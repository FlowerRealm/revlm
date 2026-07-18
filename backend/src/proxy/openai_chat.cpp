#include "proxy/openai_chat.hpp"

#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "models/models.hpp"
#include "proxy/upstream.hpp"
#include "proxy/gateway.hpp"
#include "request/request.hpp"
#include "server/http_server.hpp"
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

} // namespace

void OpenaiChatCompletion::finalize(json &json_obj)
{
    const json &root = json_obj;
    const json usage = root["usage"];
    if (!usage.is_object()) {
        return;
    }
    const long long prompt_tokens = json_int64_or(usage, "prompt_tokens");
    const long long completion_tokens = json_int64_or(usage, "completion_tokens");
    long long cached_tokens = 0;
    const json details = usage["prompt_tokens_details"];
    if (details.is_object()) {
        cached_tokens = json_int64_or(details, "cached_tokens");
    }
    const long long input_tokens = prompt_tokens > cached_tokens ? prompt_tokens - cached_tokens : 0;
    request.input_tokens = static_cast<int>(input_tokens);
    request.output_tokens = static_cast<int>(completion_tokens);
    request.cache_read_tokens = static_cast<int>(cached_tokens);
    request.cache_creation_1h_tokens = 0;
    request.cache_creation_5m_tokens = 0;
}

namespace
{

struct GatewayAttemptResult {
    int status_code = 502;
    std::vector<UpstreamHeader> response_headers;
    std::string body_bytes;
};

struct GatewayAttemptExecution {
    std::optional<GatewayAttemptResult> result;
    std::optional<GatewayAttemptTransportError> transport_error;
};

struct GatewayStreamAttemptResult {
    UpstreamStreamResponse upstream;
};

struct GatewayStreamAttemptExecution {
    std::optional<GatewayStreamAttemptResult> result;
    std::optional<GatewayAttemptTransportError> transport_error;
};

bool channel_ok_for_openai_chat(const Channel &channel)
{
    return channel.status && channel.type == "openai_compatible" && !trim_ascii(channel.api_key).empty();
}

HttpResponse proxy_upstream_failed_response(std::string_view request_id,
                                            std::string_view message = "proxy upstream failed")
{
    return http_response(502, "Bad Gateway", json{ { "error", json{ { "message", std::string{ message } } } } },
                         { { "X-Request-Id", std::string{ request_id } } });
}

GatewayAttemptExecution execute_chat_gateway_attempt(long long channel_id, const ::httplib::Request &req,
                                                     std::string_view request_id)
{
    UpstreamRequest downstream = build_proxy_upstream_request(
        req, "/v1/chat/completions", request_id, req.get_header_value("X-Revlm-Client-Ip"), req.body,
        [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });

    const ScheduledUpstreamExecution executed = execute_scheduled_upstream(channel_id, std::move(downstream));
    if (!executed.result.has_value()) {
        return GatewayAttemptExecution{ .result = std::nullopt, .transport_error = executed.transport_error };
    }
    GatewayAttemptResult result{
        .status_code = executed.result->response.status_code,
        .response_headers = executed.result->response.headers,
        .body_bytes = std::move(executed.result->response.body),
    };
    return GatewayAttemptExecution{ .result = std::move(result), .transport_error = std::nullopt };
}

GatewayStreamAttemptExecution execute_chat_gateway_stream_attempt(long long channel_id, const ::httplib::Request &req,
                                                                  std::string_view request_id)
{
    UpstreamRequest downstream = build_proxy_upstream_request(
        req, "/v1/chat/completions", request_id, req.get_header_value("X-Revlm-Client-Ip"), req.body,
        [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });

    ScheduledUpstreamStreamExecution executed = open_scheduled_upstream_stream(channel_id, std::move(downstream));
    if (!executed.result.has_value()) {
        return GatewayStreamAttemptExecution{ .result = std::nullopt, .transport_error = executed.transport_error };
    }
    return GatewayStreamAttemptExecution{
        .result =
            GatewayStreamAttemptResult{
                .upstream = std::move(*executed.result),
            },
        .transport_error = std::nullopt,
    };
}

std::optional<std::string> select_chat_proxy_model(std::string_view body)
{
    const auto model = parse_json_string_field(body, "model");
    if (!model.has_value() || trim_ascii(*model).empty()) {
        return std::nullopt;
    }
    return *model;
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
        parse_billing_request_from_body(usage, GatewayStreamKind::openai_chat, body);
    }
    usage.usd = usage.solve_price();
    usage.pricing_model = nullptr;
}

std::optional<ChannelGroup> load_chat_channel_group(long long channel_group_id)
{
    if (channel_group_id <= 0) {
        return std::nullopt;
    }
    ChannelGroup group = ChannelGroupStore::instance().get_channel_group_by_id(channel_group_id);
    if (group.id <= 0 || group.status == 0 || group.channels.empty()) {
        return std::nullopt;
    }
    if (group.pointer < 0 || group.pointer >= static_cast<int>(group.channels.size())) {
        group.pointer = 0;
    }
    return group;
}

} // namespace

HttpResponse run_chat_completions_gateway(const ::httplib::Request &req, std::string_view request_id,
                                          long long channel_group_id, Request &usage)
{
    const std::optional<std::string> model = select_chat_proxy_model(req.body);
    if (!model.has_value()) {
        return http_response(
            400, "Bad Request",
            json{ { "error",
                    json{ { "message", "chat completions model unavailable on openai-compatible channels" } } } },
            { { "X-Request-Id", std::string{ request_id } } });
    }
    if (revlm::parse_json_bool_field(req.body, "stream").value_or(false)) {
        return http_response(400, "Bad Request",
                             json{ { "error", json{ { "message", "streaming requires live socket path" } } } },
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    auto group = load_chat_channel_group(channel_group_id);
    if (!group.has_value()) {
        return http_response(400, "Bad Request",
                             json{ { "error", json{ { "message", "channel group unavailable" } } } },
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    const int start = group->pointer;
    HttpResponse last_failure = proxy_upstream_failed_response(request_id);
    bool tried = false;

    do {
        Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
        if (channel_ok_for_openai_chat(channel) && channel.find_model(*model) != nullptr) {
            tried = true;
            const auto execution = execute_chat_gateway_attempt(channel.id, req, request_id);
            if (execution.result.has_value() && execution.result->status_code < 400) {
                const GatewayAttemptResult &attempt = *execution.result;
                const std::string response_id = upstream_response_id_from_headers(attempt.response_headers);
                fill_usage_from_success(usage, channel, *model, attempt.status_code, request_id, response_id,
                                        attempt.body_bytes, false);
                return make_upstream_http_response(attempt.status_code, attempt.body_bytes,
                                                   merge_correlation_headers(attempt.response_headers, request_id,
                                                                             response_id));
            }
            if (execution.result.has_value()) {
                const GatewayAttemptResult &attempt = *execution.result;
                const std::string response_id = upstream_response_id_from_headers(attempt.response_headers);
                last_failure = make_upstream_http_response(attempt.status_code, attempt.body_bytes,
                                                           merge_correlation_headers(attempt.response_headers,
                                                                                     request_id, response_id));
                usage.channel_id = channel.id;
                usage.status_code = attempt.status_code;
            } else {
                last_failure = proxy_upstream_failed_response(request_id);
                usage.channel_id = channel.id;
                usage.status_code = 502;
            }
        }
        group->next_channel();
    } while (group->pointer != start);

    if (!tried) {
        return http_response(
            400, "Bad Request",
            json{ { "error", json{ { "message", "chat completions requires an openai-compatible channel" } } } },
            { { "X-Request-Id", std::string{ request_id } } });
    }
    return last_failure;
}

void run_chat_completions_stream(::httplib::Response &res, const ::httplib::Request &req,
                                 const GatewayParsedRequest &parsed, std::string_view request_id,
                                 long long channel_group_id, std::string_view client_ip, Request usage,
                                 const std::function<void(Request &)> &on_usage)
{
    (void)parsed;
    (void)client_ip;
    std::optional<std::string> model = select_chat_proxy_model(req.body);
    if (!model.has_value()) {
        apply_http_response(
            http_response(
                400, "Bad Request",
                json{ { "error",
                        json{ { "message", "chat completions model unavailable on openai-compatible channels" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
            res);
        return;
    }

    auto group = load_chat_channel_group(channel_group_id);
    if (!group.has_value()) {
        apply_http_response(http_response(400, "Bad Request",
                                          json{ { "error", json{ { "message", "channel group unavailable" } } } },
                                          { { "X-Request-Id", std::string{ request_id } } }),
                            res);
        return;
    }

    const int start = group->pointer;
    HttpResponse last_failure = proxy_upstream_failed_response(request_id);
    bool tried = false;

    do {
        Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
        if (channel_ok_for_openai_chat(channel) && channel.find_model(*model) != nullptr) {
            tried = true;
            GatewayStreamAttemptExecution executed = execute_chat_gateway_stream_attempt(channel.id, req, request_id);
            if (!executed.transport_error.has_value() && executed.result.has_value() &&
                executed.result->upstream.status_code < 400) {
                UpstreamStreamResponse upstream = std::move(executed.result->upstream);
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

                const std::string requested_service_tier =
                    parse_json_string_field(req.body, "service_tier").value_or("");
                const Model *pricing = usage.pricing_model;
                apply_upstream_gateway_stream(
                    res, status, upstream.headers, std::move(upstream), std::move(usage),
                    [pricing, route_mult](Request &u) -> std::unique_ptr<Gateway> {
                        if (pricing == nullptr) {
                            return nullptr;
                        }
                        return make_gateway(GatewayStreamKind::openai_chat, pricing, 1.0, route_mult, u);
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
            if (executed.transport_error.has_value()) {
                last_failure = proxy_upstream_failed_response(request_id);
                usage.channel_id = channel.id;
                usage.status_code = 502;
            } else if (executed.result.has_value()) {
                const int status = executed.result->upstream.status_code;
                const std::string error_body = drain_upstream_stream_body(executed.result->upstream);
                last_failure = make_upstream_http_response(
                    status, error_body, merge_correlation_headers(executed.result->upstream.headers, request_id, {}));
                usage.channel_id = channel.id;
                usage.status_code = status;
            }
        }
        group->next_channel();
    } while (group->pointer != start);

    if (!tried) {
        apply_http_response(
            http_response(
                400, "Bad Request",
                json{ { "error", json{ { "message", "chat completions requires an openai-compatible channel" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
            res);
        return;
    }
    apply_http_response(last_failure, res);
}

} // namespace revlm
