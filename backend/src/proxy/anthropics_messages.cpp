#include "proxy/anthropics_messages.hpp"

#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "models/models.hpp"
#include "proxy/upstream.hpp"
#include "proxy/gateway.hpp"
#include "request/request.hpp"
#include "request/proxy_request.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <functional>
#include <httplib.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revlm
{
namespace
{

bool channel_ok_for_anthropic(const Channel &channel)
{
    return channel.status && channel.type == "anthropic" && !trim_ascii(channel.api_key).empty();
}

std::optional<std::string> select_messages_proxy_model(std::string_view body)
{
    const auto model = parse_json_string_field(body, "model");
    if (!model.has_value() || trim_ascii(*model).empty()) {
        return std::nullopt;
    }
    return *model;
}

std::optional<ChannelGroup> load_messages_channel_group(long long channel_group_id)
{
    if (channel_group_id <= 0) {
        return std::nullopt;
    }
    ChannelGroup group = ChannelGroupStore::instance().get_channel_group_by_id(channel_group_id);
    if (group.id <= 0 || !group.status || group.channels.empty()) {
        return std::nullopt;
    }
    if (group.pointer < 0 || group.pointer >= static_cast<int>(group.channels.size())) {
        group.pointer = 0;
    }
    return group;
}

} // namespace

void AnthropicsMessages::finalize(json &json_obj)
{
    const json usage = json_obj["usage"].is_object() ? json_obj["usage"] : json_obj["message"]["usage"];
    const json cache_creation = usage["cache_creation"];
    request.usage.input_tokens = static_cast<int>(usage["input_tokens"].as_int64().value());
    request.usage.output_tokens = static_cast<int>(usage["output_tokens"].as_int64().value());
    request.usage.cache_read_tokens = static_cast<int>(usage["cache_read_input_tokens"].as_int64().value());
    request.usage.cache_creation_1h_tokens =
        static_cast<int>(cache_creation["ephemeral_1h_input_tokens"].as_int64().value());
    request.usage.cache_creation_5m_tokens =
        static_cast<int>(cache_creation["ephemeral_5m_input_tokens"].as_int64().value());
}

json run_messages(ProxyRequest &pr)
{
    const std::string &body = pr.http.body;
    const std::string &request_id = pr.request_id;
    const long long channel_group_id = pr.auth.channel_group_id;

    const std::optional<std::string> model = select_messages_proxy_model(body);
    if (!model.has_value()) {
        return make_proxy_error(
            400, request_id,
            json{ { "error", json{ { "message", "messages model unavailable on anthropic channels" } } } });
    }

    auto group = load_messages_channel_group(channel_group_id);
    if (!group.has_value()) {
        return make_proxy_error(400, request_id,
                                json{ { "error", json{ { "message", "channel group unavailable" } } } });
    }

    const int start = group->pointer;
    int last_status = 502;
    std::string last_body = serialize(json{ { "error", json{ { "message", "proxy upstream failed" } } } });
    std::vector<UpstreamHeader> last_headers{ { "X-Request-Id", request_id } };
    bool tried = false;

    do {
        Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
        if (channel_ok_for_anthropic(channel) && channel.find_model(*model) != nullptr) {
            tried = true;
            ScheduledUpstreamExecution executed =
                execute_scheduled_upstream(channel.id, build_proxy_upstream_request(pr, "/v1/messages"));
            if (executed.result.has_value() && executed.result->response.status_code < 400) {
                UpstreamResponse &resp = executed.result->response;
                const std::string response_id = upstream_response_id_from_headers(resp.headers);
                fill_pricing_from_model(pr.upstream.pricing, *channel.find_model(*model));
                pr.upstream.model_name = *model;
                pr.upstream.channel_id = channel.id;
                pr.upstream.status_code = resp.status_code;
                pr.upstream.channel_multiplier = channel.price_multiplier;
                pr.upstream.response_id = response_id;
                if (const auto response_tier = parse_json_string_field(resp.body, "service_tier");
                    response_tier.has_value()) {
                    pr.upstream.service_tier = normalize_usage_service_tier(std::string_view{ *response_tier });
                }
                parse_billing_request_from_body(pr, GatewayStreamKind::anthropics_messages, resp.body);
                pr.http.body.clear();
                pr.http.body.shrink_to_fit();
                return make_proxy_result(resp.status_code, std::move(resp.body),
                                         merge_correlation_headers(resp.headers, request_id, response_id));
            }
            if (executed.result.has_value()) {
                UpstreamResponse &resp = executed.result->response;
                const std::string response_id = upstream_response_id_from_headers(resp.headers);
                last_status = resp.status_code;
                last_body = std::move(resp.body);
                last_headers = merge_correlation_headers(resp.headers, request_id, response_id);
                pr.upstream.channel_id = channel.id;
                pr.upstream.status_code = resp.status_code;
            } else {
                last_status = 502;
                last_body = serialize(json{ { "error", json{ { "message", "proxy upstream failed" } } } });
                last_headers = { { "X-Request-Id", request_id } };
                pr.upstream.channel_id = channel.id;
                pr.upstream.status_code = 502;
            }
        }
        group->next_channel();
    } while (group->pointer != start);

    if (!tried) {
        return make_proxy_error(400, request_id,
                                json{ { "error", json{ { "message", "messages requires an anthropic channel" } } } });
    }
    return make_proxy_result(last_status, std::move(last_body), last_headers);
}

void run_messages_stream(::httplib::Response &res, ProxyRequest pr, const std::function<void(ProxyRequest &)> &on_usage)
{
    const std::string &body = pr.http.body;
    const std::string &request_id = pr.request_id;
    const long long channel_group_id = pr.auth.channel_group_id;

    std::optional<std::string> model = select_messages_proxy_model(body);
    if (!model.has_value()) {
        write_proxy_result(
            res, make_proxy_error(
                     400, request_id,
                     json{ { "error", json{ { "message", "messages model unavailable on anthropic channels" } } } }));
        return;
    }

    auto group = load_messages_channel_group(channel_group_id);
    if (!group.has_value()) {
        write_proxy_result(res,
                           make_proxy_error(400, request_id,
                                            json{ { "error", json{ { "message", "channel group unavailable" } } } }));
        return;
    }

    const int start = group->pointer;
    int last_status = 502;
    std::string last_body = serialize(json{ { "error", json{ { "message", "proxy upstream failed" } } } });
    std::vector<UpstreamHeader> last_headers{ { "X-Request-Id", request_id } };
    bool tried = false;

    do {
        Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
        if (channel_ok_for_anthropic(channel) && channel.find_model(*model) != nullptr) {
            tried = true;
            ScheduledUpstreamStreamExecution executed =
                open_scheduled_upstream_stream(channel.id, build_proxy_upstream_request(pr, "/v1/messages"));
            if (executed.result.has_value() && executed.result->status_code < 400) {
                UpstreamStreamResponse upstream = std::move(*executed.result);
                const int status = upstream.status_code;
                const std::string response_id = upstream_response_id_from_headers(upstream.headers);
                fill_pricing_from_model(pr.upstream.pricing, *channel.find_model(*model));
                pr.upstream.model_name = *model;
                pr.upstream.channel_id = channel.id;
                pr.upstream.status_code = status;
                pr.upstream.channel_multiplier = channel.price_multiplier;
                pr.is_stream = true;
                pr.upstream.response_id = response_id;

                const std::string requested_service_tier = parse_json_string_field(body, "service_tier").value_or("");
                pr.http.body.clear();
                pr.http.body.shrink_to_fit();
                apply_upstream_gateway_stream(
                    res, status, upstream.headers, std::move(upstream), std::move(pr),
                    [](ProxyRequest &p) -> std::unique_ptr<Gateway> {
                        return make_gateway(GatewayStreamKind::anthropics_messages, p);
                    },
                    requested_service_tier,
                    [on_usage](ProxyRequest &p, const GatewayStreamResult &result) {
                        const GatewayStreamPump &pump = result.pump;
                        const bool success = p.upstream.status_code < 400 && pump.completed && !pump.upstream_error &&
                                             !pump.idle_timeout;
                        if (!on_usage || !success || !pump.saw_usage) {
                            return;
                        }
                        p.upstream.first_token_latency_ms = pump.first_token_latency_ms;
                        on_usage(p);
                    });
                set_stream_correlation_headers(res, request_id, response_id);
                return;
            }
            if (!executed.result.has_value()) {
                last_status = 502;
                last_body = serialize(json{ { "error", json{ { "message", "proxy upstream failed" } } } });
                last_headers = { { "X-Request-Id", request_id } };
                pr.upstream.channel_id = channel.id;
                pr.upstream.status_code = 502;
            } else {
                const int status = executed.result->status_code;
                last_status = status;
                last_body = drain_upstream_stream_body(*executed.result);
                last_headers = merge_correlation_headers(executed.result->headers, request_id, {});
                pr.upstream.channel_id = channel.id;
                pr.upstream.status_code = status;
            }
        }
        group->next_channel();
    } while (group->pointer != start);

    if (!tried) {
        write_proxy_result(
            res,
            make_proxy_error(400, request_id,
                             json{ { "error", json{ { "message", "messages requires an anthropic channel" } } } }));
        return;
    }
    write_upstream(res, last_status, std::move(last_body), last_headers);
}

} // namespace revlm
