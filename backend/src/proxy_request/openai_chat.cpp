#include "proxy_request/openai_chat.hpp"

#include "channels/channels.hpp"
#include "models/models.hpp"
#include "proxy_request/api_orchestrate.hpp"
#include "proxy_request/gateway_resilience.hpp"
#include "proxy_request/upstream.hpp"
#include "proxy_response/api_stream.hpp"
#include "proxy_response/gateway.hpp"
#include "proxy_response/gateway_stream.hpp"
#include "proxy_response/upstream_http.hpp"
#include "request/request.hpp"
#include "server/http_server.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <boost/json/object.hpp>
#include <functional>
#include <httplib.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revlm
{
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

std::optional<std::string> select_chat_proxy_model(std::string_view body, long long channel_id)
{
    const auto model = parse_json_string_field(body, "model");
    if (!model.has_value()) {
        return std::nullopt;
    }

    const std::vector<Model> &catalog = ModelManager::instance().models();
    if (std::ranges::find(catalog, *model, &Model::name) == catalog.end()) {
        return std::nullopt;
    }

    const auto channel = ChannelStore::instance().find_channel(channel_id);
    if (!channel.has_value() || !channel->status || trim_ascii(channel->api_key).empty()) {
        return std::nullopt;
    }

    return *model;
}

double channel_price_multiplier(long long channel_id)
{
    const auto channel = ChannelStore::instance().find_channel(channel_id);
    return channel.has_value() ? channel->price_multiplier : 1.0;
}

void fill_usage_from_success(Request &usage, long long channel_id, std::string_view model, int status_code,
                             std::string_view request_id, std::string_view response_id, std::string_view body,
                             bool is_stream)
{
    usage.pricing_model = billing_model_for_name(model);
    usage.model_name = std::string{ model };
    usage.channel_id = channel_id;
    usage.status_code = status_code;
    usage.channel_multiplier = channel_price_multiplier(channel_id);
    usage.is_stream = is_stream;
    assign_request_correlation(usage, request_id, response_id);
    if (const auto response_tier = parse_json_string_field(body, "service_tier"); response_tier.has_value()) {
        usage.service_tier = normalize_usage_service_tier(std::string_view{ *response_tier });
    }
    if (usage.pricing_model != nullptr && !is_stream) {
        parse_billing_request_from_body(usage, GatewayStreamKind::openai_chat, body);
    }
}

} // namespace

HttpResponse run_chat_completions_gateway(const ::httplib::Request &req, std::string_view request_id,
                                          long long channel_id, Request &usage)
{
    const std::optional<std::string> model = select_chat_proxy_model(req.body, channel_id);
    if (!model.has_value()) {
        return http_response(
            400, "Bad Request",
            boost::json::object{
                { "error",
                  boost::json::object{
                      { "message", "chat completions model unavailable on openai-compatible channels" } } } },
            { { "X-Request-Id", std::string{ request_id } } });
    }
    if (revlm::parse_json_bool_field(req.body, "stream").value_or(false)) {
        return http_response(
            400, "Bad Request",
            boost::json::object{
                { "error", boost::json::object{ { "message", "streaming requires live socket path" } } } },
            { { "X-Request-Id", std::string{ request_id } } });
    }

    const auto execution = execute_chat_gateway_attempt(channel_id, req, request_id);
    if (!execution.result.has_value()) {
        const auto &transport = *execution.transport_error;
        GatewayFailure failure = classify_gateway_transport_failure(transport.stage, transport.message);
        const int failure_status = failure.status_code >= 400 ? failure.status_code : 502;
        const char *failure_reason = failure_status == 429 ? "Too Many Requests" : "Bad Gateway";
        const std::string failure_message =
            failure_status == 429 && !failure.error_message.empty() ? failure.error_message : "proxy upstream failed";
        return http_response(failure_status, failure_reason,
                             boost::json::object{ { "error", boost::json::object{ { "message", failure_message } } } },
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    const GatewayAttemptResult &attempt = *execution.result;
    const std::string response_id = upstream_response_id_from_headers(attempt.response_headers);
    if (attempt.status_code < 400) {
        fill_usage_from_success(usage, channel_id, *model, attempt.status_code, request_id, response_id,
                                attempt.body_bytes, false);
        return make_upstream_http_response(attempt.status_code, attempt.body_bytes,
                                           merge_correlation_headers(attempt.response_headers, request_id,
                                                                     response_id));
    }

    GatewayFailure failure = classify_gateway_status_failure(attempt.status_code);
    if (!failure.retriable || failure.preserve_upstream_response) {
        return make_upstream_http_response(attempt.status_code, attempt.body_bytes,
                                           merge_correlation_headers(attempt.response_headers, request_id,
                                                                     response_id));
    }

    const int failure_status = failure.status_code >= 400 ? failure.status_code : 502;
    const char *failure_reason = failure_status == 429 ? "Too Many Requests" : "Bad Gateway";
    const std::string failure_message =
        failure_status == 429 && !failure.error_message.empty() ? failure.error_message : "proxy upstream failed";
    return http_response(failure_status, failure_reason,
                         boost::json::object{ { "error", boost::json::object{ { "message", failure_message } } } },
                         { { "X-Request-Id", std::string{ request_id } } });
}

void run_chat_completions_stream(::httplib::Response &res, const ::httplib::Request &req,
                                 const GatewayParsedRequest &parsed, std::string_view request_id, long long channel_id,
                                 std::string_view client_ip, Request usage,
                                 const std::function<void(Request &)> &on_usage)
{
    (void)parsed;
    (void)client_ip;
    std::optional<std::string> model = select_chat_proxy_model(req.body, channel_id);
    if (!model.has_value()) {
        apply_http_response(
            http_response(
                400, "Bad Request",
                boost::json::object{
                    { "error",
                      boost::json::object{
                          { "message", "chat completions model unavailable on openai-compatible channels" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
            res);
        return;
    }

    GatewayStreamAttemptExecution executed = execute_chat_gateway_stream_attempt(channel_id, req, request_id);
    if (executed.transport_error.has_value()) {
        const GatewayFailure failure = classify_gateway_transport_failure(executed.transport_error->stage);
        const int failure_status = failure.status_code >= 400 ? failure.status_code : 502;
        const char *failure_reason = failure_status == 429 ? "Too Many Requests" : "Bad Gateway";
        const std::string failure_message =
            failure_status == 429 && !failure.error_message.empty() ? failure.error_message : "proxy upstream failed";
        apply_http_response(
            http_response(failure_status, failure_reason,
                          boost::json::object{ { "error", boost::json::object{ { "message", failure_message } } } },
                          { { "X-Request-Id", std::string{ request_id } } }),
            res);
        return;
    }

    UpstreamStreamResponse upstream = std::move(executed.result->upstream);
    const int status = upstream.status_code;
    const double route_mult = channel_price_multiplier(channel_id);
    if (status >= 400) {
        GatewayFailure failure = classify_gateway_status_failure(status);
        if (failure.retriable) {
            const int failure_status = failure.status_code >= 400 ? failure.status_code : 502;
            const char *failure_reason = failure_status == 429 ? "Too Many Requests" : "Bad Gateway";
            const std::string failure_message = failure_status == 429 && !failure.error_message.empty() ?
                                                    failure.error_message :
                                                    "proxy upstream failed";
            apply_http_response(
                http_response(failure_status, failure_reason,
                              boost::json::object{ { "error", boost::json::object{ { "message", failure_message } } } },
                              { { "X-Request-Id", std::string{ request_id } } }),
                res);
            return;
        }
    }

    const std::string response_id = upstream_response_id_from_headers(upstream.headers);
    usage.pricing_model = billing_model_for_name(*model);
    usage.model_name = *model;
    usage.channel_id = channel_id;
    usage.status_code = status;
    usage.channel_multiplier = route_mult;
    usage.is_stream = true;
    assign_request_correlation(usage, request_id, response_id);

    const std::string requested_service_tier = parse_json_string_field(req.body, "service_tier").value_or("");
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
            const bool success = status < 400 && pump.completed && !pump.upstream_error && !pump.idle_timeout;
            if (!on_usage || !success || !pump.saw_usage || u.pricing_model == nullptr) {
                return;
            }
            u.first_token_latency_ms = pump.first_token_latency_ms;
            on_usage(u);
        });
    set_stream_correlation_headers(res, request_id, response_id);
}

} // namespace revlm
