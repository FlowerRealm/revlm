#include "proxy/openai_chat.hpp"

#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "models/models.hpp"
#include "proxy/upstream.hpp"
#include "proxy/gateway.hpp"
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

void OpenaiChatCompletion::finalize(json &json_obj)
{
    // Official CompletionUsage: prompt_tokens includes cached_tokens (subset);
    // prompt_tokens_details may omit fields — treat nested as optional.
    const json usage = json_obj["usage"];
    const long long prompt_tokens = usage["prompt_tokens"].as_int64().value();
    const long long completion_tokens = usage["completion_tokens"].as_int64().value();
    const json details = usage["prompt_tokens_details"];
    const long long cached_tokens = details.is_object() ? details["cached_tokens"].as_int64().value_or(0) : 0;
    const long long cache_write_tokens = details.is_object() ? details["cache_write_tokens"].as_int64().value_or(0) : 0;
    request.usage.input_tokens = static_cast<int>(prompt_tokens - cached_tokens);
    request.usage.output_tokens = static_cast<int>(completion_tokens);
    request.usage.cache_read_tokens = static_cast<int>(cached_tokens);
    request.usage.cache_creation_1h_tokens = 0; // OpenAI has no 1h/5m split
    request.usage.cache_creation_5m_tokens = static_cast<int>(cache_write_tokens);
    if (const auto tier = json_obj["service_tier"].as_string(); tier.has_value()) {
        request.upstream.service_tier = normalize_usage_service_tier(std::string_view{ *tier });
    }
    if (const auto model = json_obj["model"].as_string(); model.has_value() && !model->empty()) {
        request.upstream.model_name = *model;
    }
}

namespace
{

bool channel_ok_for_openai_chat(const Channel &channel)
{
    return channel.status && channel.type == "openai_compatible" && !trim_ascii(channel.api_key).empty();
}

std::optional<ChannelGroup> load_chat_channel_group(long long channel_group_id)
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

json run_chat_completions(ProxyRequest &pr)
{
    const std::string request_id = pr.request_id;
    const long long channel_group_id = pr.auth.channel_group_id;

    auto group = load_chat_channel_group(channel_group_id);
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
        if (channel_ok_for_openai_chat(channel)) {
            tried = true;
            ScheduledUpstreamExecution executed =
                execute_scheduled_upstream(channel.id, build_proxy_upstream_request(pr, "/v1/chat/completions"));
            if (executed.result.has_value() && executed.result->response.status_code < 400) {
                UpstreamResponse &resp = executed.result->response;
                const std::string response_id = upstream_response_id_from_headers(resp.headers);
                pr.upstream.channel_id = channel.id;
                pr.upstream.model_name = parse_json_string_field(resp.body, "model").value_or("");
                if (const Model *billing_model = channel.find_model(pr.upstream.model_name)) {
                    fill_pricing_from_model(pr.upstream.pricing, *billing_model);
                }
                pr.upstream.status_code = resp.status_code;
                pr.upstream.channel_multiplier = channel.price_multiplier;
                pr.upstream.response_id = response_id;
                assign_request_correlation(pr, request_id, response_id);
                if (const auto response_tier = parse_json_string_field(resp.body, "service_tier");
                    response_tier.has_value()) {
                    pr.upstream.service_tier = normalize_usage_service_tier(std::string_view{ *response_tier });
                }
                parse_billing_request_from_body(pr, GatewayStreamKind::openai_chat, resp.body);
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
                                json{ { "error", json{ { "message", "no available openai-compatible channel" } } } });
    }
    return make_proxy_result(last_status, std::move(last_body), last_headers);
}

void run_chat_completions_stream(::httplib::Response &res, ProxyRequest pr,
                                 const std::function<void(ProxyRequest &)> &on_usage)
{
    const std::string request_id = pr.request_id;
    const long long channel_group_id = pr.auth.channel_group_id;

    auto group = load_chat_channel_group(channel_group_id);
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
        if (channel_ok_for_openai_chat(channel)) {
            tried = true;
            ScheduledUpstreamStreamExecution executed =
                open_scheduled_upstream_stream(channel.id, build_proxy_upstream_request(pr, "/v1/chat/completions"));
            if (executed.result.has_value() && executed.result->status_code < 400) {
                UpstreamStreamResponse upstream = std::move(*executed.result);
                const int status = upstream.status_code;
                const std::string response_id = upstream_response_id_from_headers(upstream.headers);
                const long long channel_id = channel.id;
                const double route_mult = channel.price_multiplier;
                pr.upstream.channel_id = channel_id;
                pr.upstream.status_code = status;
                pr.upstream.channel_multiplier = route_mult;
                pr.upstream.response_id = response_id;
                assign_request_correlation(pr, request_id, response_id);

                pr.http.body.clear();
                pr.http.body.shrink_to_fit();
                apply_upstream_gateway_stream(
                    res, status, upstream.headers, std::move(upstream), std::move(pr),
                    [](ProxyRequest &u) -> std::unique_ptr<Gateway> {
                        return make_gateway(GatewayStreamKind::openai_chat, u);
                    },
                    [status, on_usage, channel_id, route_mult](ProxyRequest &u, const GatewayStreamResult &result) {
                        const GatewayStreamPump &pump = result.pump;
                        const bool success = status < 400 && pump.completed && !pump.upstream_error &&
                                             !pump.idle_timeout;
                        if (!on_usage || !success || !pump.saw_usage) {
                            return;
                        }
                        // finalize() already set model/usage from official SSE chunks.
                        if (const auto channel = ChannelStore::instance().find_channel(channel_id);
                            channel.has_value()) {
                            u.upstream.channel_multiplier = route_mult;
                            if (const Model *model = channel->find_model(u.upstream.model_name)) {
                                fill_pricing_from_model(u.upstream.pricing, *model);
                            }
                        }
                        u.upstream.first_token_latency_ms = pump.first_token_latency_ms;
                        on_usage(u);
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
                             json{ { "error", json{ { "message", "no available openai-compatible channel" } } } }));
        return;
    }
    write_upstream(res, last_status, std::move(last_body), last_headers);
}

} // namespace revlm
