#include "proxy/anthropics_messages.hpp"

#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "models/models.hpp"
#include "proxy/upstream.hpp"
#include "proxy/gateway.hpp"
#include "request/request.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <algorithm>
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

long long json_int64_or(const json &obj, std::string_view key, long long fallback = 0)
{
    if (!obj.is_object() || !obj.contains(key)) {
        return fallback;
    }
    return static_cast<const json &>(obj)[key].as_int64().value_or(fallback);
}

std::optional<json> extract_usage_object(const json &value)
{
    if (value.is_object()) {
        const json usage = value["usage"];
        if (usage.is_object()) {
            return usage;
        }
        for (const auto &key : value.keys()) {
            if (auto nested = extract_usage_object(value[key])) {
                return nested;
            }
        }
        return std::nullopt;
    }
    if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (auto nested = extract_usage_object(value[i])) {
                return nested;
            }
        }
    }
    return std::nullopt;
}

int merge_token(int current, long long incoming)
{
    if (incoming > 0) {
        return static_cast<int>(incoming);
    }
    return current;
}

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

void fill_usage_from_success(Request &usage, const Channel &channel, std::string_view model, int status_code,
                             std::string_view request_id, std::string_view response_id, std::string_view body,
                             bool is_stream)
{
    usage.pricing_model = channel.find_model(model);
    usage.model_name = std::string{ model };
    usage.channel_id = channel.id;
    usage.status_code = status_code;
    usage.channel_multiplier = channel.price_multiplier;
    usage.is_stream = is_stream;
    assign_request_correlation(usage, request_id, response_id);
    if (const auto response_tier = parse_json_string_field(body, "service_tier"); response_tier.has_value()) {
        usage.service_tier = normalize_usage_service_tier(std::string_view{ *response_tier });
    }
    if (usage.pricing_model != nullptr && !is_stream) {
        parse_billing_request_from_body(usage, GatewayStreamKind::anthropics_messages, body);
    }
    usage.usd = usage.solve_price();
    usage.pricing_model = nullptr;
}

} // namespace

void AnthropicsMessages::finalize(json &json_obj)
{
    auto usage_opt = extract_usage_object(json_obj);
    if (!usage_opt.has_value()) {
        return;
    }
    const json &usage = *usage_opt;
    long long ephemeral_1h = 0;
    long long ephemeral_5m = 0;
    const json cache_creation = usage["cache_creation"];
    if (cache_creation.is_object()) {
        ephemeral_1h = json_int64_or(cache_creation, "ephemeral_1h_input_tokens");
        ephemeral_5m = json_int64_or(cache_creation, "ephemeral_5m_input_tokens");
    }
    const int input_tokens = merge_token(request.input_tokens, json_int64_or(usage, "input_tokens"));
    const int output_tokens = merge_token(request.output_tokens, json_int64_or(usage, "output_tokens"));
    const int cache_read_tokens =
        merge_token(request.cache_read_tokens, json_int64_or(usage, "cache_read_input_tokens"));
    const int cache_creation_1h_tokens = merge_token(request.cache_creation_1h_tokens, ephemeral_1h);
    const int cache_creation_5m_tokens = merge_token(request.cache_creation_5m_tokens, ephemeral_5m);
    request.input_tokens = input_tokens;
    request.output_tokens = output_tokens;
    request.cache_read_tokens = cache_read_tokens;
    request.cache_creation_1h_tokens = cache_creation_1h_tokens;
    request.cache_creation_5m_tokens = cache_creation_5m_tokens;
}

json run_messages(json req, Request &usage)
{
    const std::string body = req["body"].as_string().value_or("");
    const std::string request_id = req["request_id"].as_string().value_or("");
    const long long channel_group_id = req["channel_group_id"].as_int64().value_or(0);

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
                execute_scheduled_upstream(channel.id, build_proxy_upstream_request(req, "/v1/messages"));
            if (executed.result.has_value() && executed.result->response.status_code < 400) {
                UpstreamResponse &resp = executed.result->response;
                const std::string response_id = upstream_response_id_from_headers(resp.headers);
                fill_usage_from_success(usage, channel, *model, resp.status_code, request_id, response_id, resp.body,
                                        false);
                return make_proxy_result(resp.status_code, std::move(resp.body),
                                         merge_correlation_headers(resp.headers, request_id, response_id));
            }
            if (executed.result.has_value()) {
                UpstreamResponse &resp = executed.result->response;
                const std::string response_id = upstream_response_id_from_headers(resp.headers);
                last_status = resp.status_code;
                last_body = std::move(resp.body);
                last_headers = merge_correlation_headers(resp.headers, request_id, response_id);
                usage.channel_id = channel.id;
                usage.status_code = resp.status_code;
            } else {
                last_status = 502;
                last_body = serialize(json{ { "error", json{ { "message", "proxy upstream failed" } } } });
                last_headers = { { "X-Request-Id", request_id } };
                usage.channel_id = channel.id;
                usage.status_code = 502;
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

void run_messages_stream(::httplib::Response &res, json req, Request usage,
                         const std::function<void(Request &)> &on_usage)
{
    const std::string body = req["body"].as_string().value_or("");
    const std::string request_id = req["request_id"].as_string().value_or("");
    const long long channel_group_id = req["channel_group_id"].as_int64().value_or(0);

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
                open_scheduled_upstream_stream(channel.id, build_proxy_upstream_request(req, "/v1/messages"));
            if (executed.result.has_value() && executed.result->status_code < 400) {
                UpstreamStreamResponse upstream = std::move(*executed.result);
                const int status = upstream.status_code;
                const double route_mult = channel.price_multiplier;
                const std::string response_id = upstream_response_id_from_headers(upstream.headers);
                usage.pricing_model = channel.find_model(*model);
                usage.model_name = *model;
                usage.channel_id = channel.id;
                usage.status_code = status;
                usage.channel_multiplier = route_mult;
                usage.is_stream = true;
                assign_request_correlation(usage, request_id, response_id);

                const std::string requested_service_tier = parse_json_string_field(body, "service_tier").value_or("");
                const Model *pricing = usage.pricing_model;
                apply_upstream_gateway_stream(
                    res, status, upstream.headers, std::move(upstream), std::move(usage),
                    [pricing, route_mult](Request &u) -> std::unique_ptr<Gateway> {
                        if (pricing == nullptr) {
                            return nullptr;
                        }
                        return make_gateway(GatewayStreamKind::anthropics_messages, pricing, 1.0, route_mult, u);
                    },
                    requested_service_tier,
                    [status, on_usage](Request &u, const GatewayStreamResult &result) {
                        const GatewayStreamPump &pump = result.pump;
                        const bool success = status < 400 && pump.completed && !pump.upstream_error &&
                                             !pump.idle_timeout;
                        if (!on_usage || !success || !pump.saw_usage || u.pricing_model == nullptr) {
                            return;
                        }
                        u.first_token_latency_ms = pump.first_token_latency_ms;
                        u.usd = u.solve_price();
                        u.pricing_model = nullptr;
                        on_usage(u);
                    });
                set_stream_correlation_headers(res, request_id, response_id);
                return;
            }
            if (!executed.result.has_value()) {
                last_status = 502;
                last_body = serialize(json{ { "error", json{ { "message", "proxy upstream failed" } } } });
                last_headers = { { "X-Request-Id", request_id } };
                usage.channel_id = channel.id;
                usage.status_code = 502;
            } else {
                const int status = executed.result->status_code;
                last_status = status;
                last_body = drain_upstream_stream_body(*executed.result);
                last_headers = merge_correlation_headers(executed.result->headers, request_id, {});
                usage.channel_id = channel.id;
                usage.status_code = status;
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
