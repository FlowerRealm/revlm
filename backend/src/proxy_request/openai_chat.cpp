#include "proxy_request/openai_chat.hpp"

#include "config/config.hpp"
#include "models/models.hpp"
#include "proxy_request/api_orchestrate.hpp"
#include "proxy_request/gateway_resilience.hpp"
#include "proxy_request/upstream.hpp"
#include "proxy_response/api_stream.hpp"
#include "proxy_response/upstream_http.hpp"
#include "scheduler/scheduler.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <boost/json.hpp>
#include <httplib.h>

#include <algorithm>
#include <chrono>
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

constexpr std::string_view gateway_type_openai = "openai_compatible";

struct SchedulerChatSelection {
    SchedulerSelection selection;
    std::string requested_model;
    std::string forwarded_model;
};

struct GatewayAttemptResult {
    SchedulerChatSelection selection;
    int status_code = 502;
    std::vector<UpstreamHeader> response_headers;
    std::string body_bytes;
};

struct GatewayAttemptExecution {
    std::optional<GatewayAttemptResult> result;
    std::optional<GatewayAttemptTransportError> transport_error;
};

struct GatewayStreamAttemptResult {
    SchedulerChatSelection selection;
    UpstreamStreamResponse upstream;
};

struct GatewayStreamAttemptExecution {
    std::optional<GatewayStreamAttemptResult> result;
    std::optional<GatewayAttemptTransportError> transport_error;
};

GatewayAttemptExecution execute_chat_gateway_attempt(const SchedulerChatSelection &selection,
                                                     const ::httplib::Request &req, std::string_view request_id)
{
    const std::string body = selection.requested_model == selection.forwarded_model ?
                                 req.body :
                                 replace_json_string_field(req.body, "model", selection.forwarded_model);
    const UpstreamRequest downstream = build_proxy_upstream_request(
        req, "/v1/chat/completions", request_id, req.get_header_value("X-Revlm-Client-Ip"), body,
        [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });

    const ScheduledUpstreamExecution executed = execute_scheduled_upstream(selection.selection, downstream);
    if (!executed.result.has_value()) {
        return GatewayAttemptExecution{ .result = std::nullopt, .transport_error = executed.transport_error };
    }
    GatewayAttemptResult result{
        .selection = selection,
        .status_code = executed.result->response.status_code,
        .response_headers = executed.result->response.headers,
        .body_bytes = executed.result->response.body,
    };
    return GatewayAttemptExecution{ .result = std::move(result), .transport_error = std::nullopt };
}

GatewayStreamAttemptExecution execute_chat_gateway_stream_attempt(const SchedulerChatSelection &selection,
                                                                  const ::httplib::Request &req,
                                                                  std::string_view request_id)
{
    const std::string body = selection.requested_model == selection.forwarded_model ?
                                 req.body :
                                 replace_json_string_field(req.body, "model", selection.forwarded_model);
    const UpstreamRequest downstream = build_proxy_upstream_request(
        req, "/v1/chat/completions", request_id, req.get_header_value("X-Revlm-Client-Ip"), body,
        [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });

    const ScheduledUpstreamStreamExecution executed = open_scheduled_upstream_stream(selection.selection, downstream);
    if (!executed.result.has_value()) {
        return GatewayStreamAttemptExecution{ .result = std::nullopt, .transport_error = executed.transport_error };
    }
    return GatewayStreamAttemptExecution{
        .result =
            GatewayStreamAttemptResult{
                .selection = selection,
                .upstream = std::move(*executed.result),
            },
        .transport_error = std::nullopt,
    };
}

std::optional<SchedulerChatSelection>
select_chat_proxy_target_with_scheduler(std::string_view body, ProxyGatewayContext &gateway, long long channel_id)
{
    const auto requested_model = parse_json_string_field(body, "model");
    if (!requested_model.has_value()) {
        return std::nullopt;
    }

    std::string forwarded_model = *requested_model;
    const std::vector<Model> &models = ModelManager::instance().models();
    if (std::ranges::find(models, forwarded_model, &Model::name) == models.end()) {
        return std::nullopt;
    }
    gateway.scheduler.set_cooldown_base(std::chrono::milliseconds(config().gateway_retry_base_delay_ms));

    const SchedulerConstraints constraints =
        build_scheduler_constraints(channel_id, forwarded_model, SchedulerApi::openai);

    const std::string route_key = std::to_string(channel_id) + ":" + forwarded_model;
    SchedulerSelection scheduled;
    try {
        scheduled = gateway.scheduler.select(channel_id, gateway.scheduler.route_key_hash(route_key), constraints);
    } catch (const std::runtime_error &) {
        return std::nullopt;
    }
    if (scheduled.channel_type != gateway_type_openai) {
        return std::nullopt;
    }

    if (trim_ascii(scheduled.api_key).empty()) {
        return std::nullopt;
    }

    return SchedulerChatSelection{
        .selection = std::move(scheduled),
        .requested_model = *requested_model,
        .forwarded_model = forwarded_model,
    };
}

ResponsesProxyResult usage_only_failure(std::string_view forwarded_model, long long channel_id, int status_code,
                                        std::string_view response_id, const GatewayFailure &failure,
                                        HttpResponse response)
{
    ResponsesProxyResult out;
    out.response = std::move(response);
    out.has_usage = true;
    out.billable = false;
    out.forwarded_model = std::string{ forwarded_model };
    out.channel_id = channel_id;
    out.status_code = status_code;
    out.response_id = std::string{ response_id };
    out.is_stream = false;
    out.error_class = failure.error_class;
    out.error_message = failure.error_message;
    return out;
}

} // namespace

ResponsesProxyResult run_chat_completions_gateway(const ::httplib::Request &req, std::string_view request_id,
                                                  long long channel_id)
{
    ProxyGatewayContext gateway;
    const std::optional<SchedulerChatSelection> selection =
        select_chat_proxy_target_with_scheduler(req.body, gateway, channel_id);
    if (!selection.has_value()) {
        return ResponsesProxyResult{
            .response = http_response(
                400, "Bad Request",
                boost::json::object{
                    { "error",
                      boost::json::object{
                          { "message", "chat completions model unavailable on openai-compatible channels" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
        };
    }
    if (revlm::parse_json_bool_field(req.body, "stream").value_or(false)) {
        return ResponsesProxyResult{
            .response = http_response(
                400, "Bad Request",
                boost::json::object{
                    { "error", boost::json::object{ { "message", "streaming requires live socket path" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
        };
    }

    const auto execution = execute_chat_gateway_attempt(*selection, req, request_id);
    if (!execution.result.has_value()) {
        const auto &transport = *execution.transport_error;
        GatewayFailure failure = classify_gateway_transport_failure(transport.stage, transport.message);
        gateway.scheduler.report(selection->selection, gateway_failure_to_scheduler_result(failure));
        const int failure_status = failure.status_code >= 400 ? failure.status_code : 502;
        const char *failure_reason = failure_status == 429 ? "Too Many Requests" : "Bad Gateway";
        const std::string failure_message =
            failure_status == 429 && !failure.error_message.empty() ? failure.error_message : "proxy upstream failed";
        return usage_only_failure(
            selection->forwarded_model, selection->selection.channel_id, failure.status_code, "", failure,
            http_response(failure_status, failure_reason,
                          boost::json::object{ { "error", boost::json::object{ { "message", failure_message } } } },
                          { { "X-Request-Id", std::string{ request_id } } }));
    }

    const GatewayAttemptResult &attempt = *execution.result;
    const std::string response_id = upstream_response_id_from_headers(attempt.response_headers);
    if (attempt.status_code < 400) {
        SchedulerResult ok;
        ok.success = true;
        gateway.scheduler.report(selection->selection, ok);
        ResponsesProxyResult out;
        out.response =
            make_upstream_http_response(attempt.status_code, attempt.body_bytes,
                                        merge_correlation_headers(attempt.response_headers, request_id, response_id));
        out.has_usage = true;
        out.billable = true;
        out.forwarded_model = attempt.selection.forwarded_model;
        out.channel_id = attempt.selection.selection.channel_id;
        out.status_code = attempt.status_code;
        out.response_body = attempt.body_bytes;
        out.channel_multiplier = attempt.selection.selection.route_group_multiplier;
        out.response_id = response_id;
        out.is_stream = false;
        if (const auto response_tier = parse_json_string_field(attempt.body_bytes, "service_tier");
            response_tier.has_value()) {
            out.service_tier = *response_tier;
        }
        return out;
    }

    GatewayFailure failure = classify_gateway_status_failure(attempt.status_code);
    gateway.scheduler.report(selection->selection, gateway_failure_to_scheduler_result(failure));
    if (!failure.retriable || failure.preserve_upstream_response) {
        return ResponsesProxyResult{
            .response = make_upstream_http_response(attempt.status_code, attempt.body_bytes,
                                                    merge_correlation_headers(attempt.response_headers, request_id,
                                                                              response_id)),
        };
    }

    const int failure_status = failure.status_code >= 400 ? failure.status_code : 502;
    const char *failure_reason = failure_status == 429 ? "Too Many Requests" : "Bad Gateway";
    const std::string failure_message =
        failure_status == 429 && !failure.error_message.empty() ? failure.error_message : "proxy upstream failed";
    return usage_only_failure(
        selection->forwarded_model, selection->selection.channel_id, failure.status_code, response_id, failure,
        http_response(failure_status, failure_reason,
                      boost::json::object{ { "error", boost::json::object{ { "message", failure_message } } } },
                      { { "X-Request-Id", std::string{ request_id } } }));
}

void run_chat_completions_stream(::httplib::Response &res, const ::httplib::Request &req,
                                 const GatewayParsedRequest &parsed, std::string_view request_id, long long channel_id,
                                 std::string_view client_ip,
                                 const std::function<void(ResponsesProxyResult)> &on_stream_usage)
{
    (void)parsed;
    (void)client_ip;
    ProxyGatewayContext gateway;
    const std::optional<SchedulerChatSelection> selection =
        select_chat_proxy_target_with_scheduler(req.body, gateway, channel_id);
    if (!selection.has_value()) {
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

    const GatewayStreamAttemptExecution executed = execute_chat_gateway_stream_attempt(*selection, req, request_id);
    if (executed.transport_error.has_value()) {
        const GatewayFailure failure = classify_gateway_transport_failure(executed.transport_error->stage);
        gateway.scheduler.report(selection->selection, gateway_failure_to_scheduler_result(failure));
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
    if (status >= 400) {
        GatewayFailure failure = classify_gateway_status_failure(status);
        if (failure.retriable) {
            gateway.scheduler.report(selection->selection, gateway_failure_to_scheduler_result(failure));
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

    const SchedulerChatSelection committed = *selection;
    const std::string response_id = upstream_response_id_from_headers(upstream.headers);
    const auto stream_billing_model = billing_model_for_name(committed.forwarded_model);
    const std::string requested_service_tier = parse_json_string_field(req.body, "service_tier").value_or("");
    std::unique_ptr<Gateway> stream_gateway;
    if (stream_billing_model.has_value()) {
        stream_gateway = make_gateway(GatewayStreamKind::openai_chat, *stream_billing_model, 1.0,
                                      committed.selection.route_group_multiplier);
    }
    apply_upstream_gateway_stream(
        res, status, upstream.headers, std::move(upstream), std::move(stream_gateway), requested_service_tier, 0,
        [committed, request_id = std::string(request_id), response_id, status,
         on_stream_usage](const GatewayStreamResult &result) {
            const GatewayStreamPump &pump = result.pump;
            ProxyGatewayContext report_gateway;
            SchedulerResult scheduler_result;
            scheduler_result.success = status < 400 && pump.completed && !pump.upstream_error && !pump.idle_timeout;
            if (!scheduler_result.success) {
                scheduler_result = gateway_failure_to_scheduler_result(classify_gateway_stream_failure(pump, status));
            }
            report_gateway.scheduler.report(committed.selection, scheduler_result);

            if (!on_stream_usage || !scheduler_result.success || !result.billing_request.has_value()) {
                return;
            }
            ResponsesProxyResult usage;
            usage.has_usage = true;
            usage.billable = true;
            usage.forwarded_model = committed.forwarded_model;
            usage.channel_id = committed.selection.channel_id;
            usage.status_code = status;
            usage.channel_multiplier = committed.selection.route_group_multiplier;
            usage.response_id = response_id;
            usage.is_stream = true;
            usage.first_token_latency_ms = pump.first_token_latency_ms;
            usage.billing_request = *result.billing_request;
            on_stream_usage(std::move(usage));
        });
    set_stream_correlation_headers(res, request_id, response_id);
}

} // namespace revlm
