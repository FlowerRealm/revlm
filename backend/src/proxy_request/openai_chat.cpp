#include "proxy_request/openai_chat.hpp"

#include "config/config.hpp"
#include "models/models.hpp"
#include "proxy_request/api_orchestrate.hpp"
#include "proxy_request/gateway_resilience.hpp"
#include "proxy_request/token_auth.hpp"
#include "proxy_request/upstream.hpp"
#include "proxy_response/api_stream.hpp"
#include "proxy_response/upstream_http.hpp"
#include "request/request.hpp"
#include "scheduler/scheduler.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <boost/json.hpp>
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <iostream>
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
    TokenAuth auth;
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
select_chat_proxy_target_with_scheduler(std::string_view body, ProxyGatewayContext &gateway, const TokenAuth &auth)
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
        build_scheduler_constraints(auth.channel_id, forwarded_model, SchedulerApi::openai);

    const std::string route_key =
        std::to_string(auth.user_id) + ":" + std::to_string(auth.token_id) + ":" + forwarded_model;
    SchedulerSelection scheduled;
    try {
        scheduled = gateway.scheduler.select(auth.user_id, gateway.scheduler.route_key_hash(route_key), constraints);
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
        .auth = auth,
        .selection = std::move(scheduled),
        .requested_model = *requested_model,
        .forwarded_model = forwarded_model,
    };
}

} // namespace

HttpResponse run_chat_completions_gateway(const ::httplib::Request &req, std::string_view request_id,
                                          long long usage_event_id)
{
    const boost::json::object auth_result = authenticate_token(req);
    if (!auth_result.at("status").as_bool()) {
        return http_response(401, "Unauthorized",
                             boost::json::object{ { "error", boost::json::object{ { "message", "Unauthorized" } } } },
                             { { "X-Request-Id", std::string{ request_id } } });
    }
    const boost::json::object &auth_obj = auth_result.at("auth").as_object();
    const TokenAuth auth{
        .user_id = auth_obj.at("user_id").as_int64(),
        .token_id = auth_obj.at("token_id").as_int64(),
        .role = std::string(auth_obj.at("role").as_string()),
        .channel_id = auth_obj.at("channel_id").as_int64(),
    };

    ProxyGatewayContext gateway;
    const std::optional<SchedulerChatSelection> selection =
        select_chat_proxy_target_with_scheduler(req.body, gateway, auth);
    if (!selection.has_value()) {
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
    if (const auto quota_error = paygo_balance_gate(auth.user_id, request_id); quota_error.has_value()) {
        return *quota_error;
    }

    const auto execution = execute_chat_gateway_attempt(*selection, req, request_id);
    if (!execution.result.has_value()) {
        const auto &transport = *execution.transport_error;
        GatewayFailure failure = classify_gateway_transport_failure(transport.stage, transport.message);
        gateway.scheduler.report(selection->selection, gateway_failure_to_scheduler_result(failure));
        Request usage_request = make_proxy_usage_request(selection->auth, selection->forwarded_model,
                                                         "/v1/chat/completions", usage_event_id,
                                                         selection->selection.channel_id, failure.status_code, false);
        assign_request_correlation(usage_request, request_id, "");
        if (!failure.error_class.empty()) {
            usage_request.error_class = failure.error_class;
        }
        if (!failure.error_message.empty()) {
            usage_request.error_message = failure.error_message;
        }
        (void)commit_proxy_usage(usage_request, nullptr);
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
        SchedulerResult ok;
        ok.success = true;
        gateway.scheduler.report(selection->selection, ok);
        Request usage_request = make_proxy_usage_request(attempt.selection.auth, attempt.selection.forwarded_model,
                                                         "/v1/chat/completions", usage_event_id,
                                                         attempt.selection.selection.channel_id, attempt.status_code,
                                                         false);
        assign_request_correlation(usage_request, request_id, response_id);
        if (const auto response_tier = parse_json_string_field(attempt.body_bytes, "service_tier");
            response_tier.has_value()) {
            usage_request.service_tier = *response_tier;
        }
        std::unique_ptr<Request> billing_request;
        if (const auto billing_model = billing_model_for_name(attempt.selection.forwarded_model);
            billing_model.has_value()) {
            billing_request = std::make_unique<Request>(parse_billing_request_from_body(
                GatewayStreamKind::openai_chat, *billing_model, attempt.selection.auth.user_id, attempt.body_bytes, 1.0,
                attempt.selection.selection.route_group_multiplier));
        }
        if (!commit_proxy_usage(usage_request, billing_request.get())) {
            return http_response(
                502, "Bad Gateway",
                boost::json::object{ { "error", boost::json::object{ { "message", "usage commit failed" } } } },
                { { "X-Request-Id", std::string{ request_id } } });
        }
        return make_upstream_http_response(attempt.status_code, attempt.body_bytes,
                                           merge_correlation_headers(attempt.response_headers, request_id,
                                                                     response_id));
    }

    GatewayFailure failure = classify_gateway_status_failure(attempt.status_code);
    gateway.scheduler.report(selection->selection, gateway_failure_to_scheduler_result(failure));
    if (!failure.retriable || failure.preserve_upstream_response) {
        return make_upstream_http_response(attempt.status_code, attempt.body_bytes,
                                           merge_correlation_headers(attempt.response_headers, request_id,
                                                                     response_id));
    }

    Request usage_request = make_proxy_usage_request(selection->auth, selection->forwarded_model,
                                                     "/v1/chat/completions", usage_event_id,
                                                     selection->selection.channel_id, failure.status_code, false);
    assign_request_correlation(usage_request, request_id, response_id);
    if (!failure.error_class.empty()) {
        usage_request.error_class = failure.error_class;
    }
    if (!failure.error_message.empty()) {
        usage_request.error_message = failure.error_message;
    }
    (void)commit_proxy_usage(usage_request, nullptr);
    const int failure_status = failure.status_code >= 400 ? failure.status_code : 502;
    const char *failure_reason = failure_status == 429 ? "Too Many Requests" : "Bad Gateway";
    const std::string failure_message =
        failure_status == 429 && !failure.error_message.empty() ? failure.error_message : "proxy upstream failed";
    return http_response(failure_status, failure_reason,
                         boost::json::object{ { "error", boost::json::object{ { "message", failure_message } } } },
                         { { "X-Request-Id", std::string{ request_id } } });
}

void run_chat_completions_stream(::httplib::Response &res, const ::httplib::Request &req,
                                 const GatewayParsedRequest &parsed, std::string_view request_id,
                                 long long usage_event_id, std::string_view client_ip)
{
    (void)parsed;
    (void)client_ip;
    const boost::json::object auth_result = authenticate_token(req);
    if (!auth_result.at("status").as_bool()) {
        apply_http_response(
            http_response(401, "Unauthorized",
                          boost::json::object{ { "error", boost::json::object{ { "message", "Unauthorized" } } } },
                          { { "X-Request-Id", std::string{ request_id } } }),
            res);
        return;
    }
    const boost::json::object &auth_obj = auth_result.at("auth").as_object();
    const TokenAuth auth{
        .user_id = auth_obj.at("user_id").as_int64(),
        .token_id = auth_obj.at("token_id").as_int64(),
        .role = std::string(auth_obj.at("role").as_string()),
        .channel_id = auth_obj.at("channel_id").as_int64(),
    };

    ProxyGatewayContext gateway;
    const std::optional<SchedulerChatSelection> selection =
        select_chat_proxy_target_with_scheduler(req.body, gateway, auth);
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
    if (const auto quota_error = paygo_balance_gate(auth.user_id, request_id); quota_error.has_value()) {
        apply_http_response(*quota_error, res);
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
        res, status, upstream.headers, std::move(upstream), std::move(stream_gateway), requested_service_tier,
        committed.auth.user_id,
        [committed, request_id = std::string(request_id), response_id, usage_event_id,
         status](const GatewayStreamResult &result) {
            try {
                const GatewayStreamPump &pump = result.pump;
                ProxyGatewayContext gateway;
                SchedulerResult scheduler_result;
                scheduler_result.success = status < 400 && pump.completed && !pump.upstream_error && !pump.idle_timeout;
                std::optional<GatewayFailure> stream_failure;
                if (!scheduler_result.success) {
                    stream_failure = classify_gateway_stream_failure(pump, status);
                    scheduler_result = gateway_failure_to_scheduler_result(*stream_failure);
                }
                gateway.scheduler.report(committed.selection, scheduler_result);

                if (!scheduler_result.success || !result.billing_request.has_value()) {
                    return;
                }
                Request usage_request = make_proxy_usage_request(committed.auth, committed.forwarded_model,
                                                                 "/v1/chat/completions", usage_event_id,
                                                                 committed.selection.channel_id, status, true);
                assign_request_correlation(usage_request, request_id, response_id);
                usage_request.first_token_latency_ms = pump.first_token_latency_ms;
                std::unique_ptr<Request> billing_request = std::make_unique<Request>(*result.billing_request);
                if (!commit_proxy_usage(usage_request, billing_request.get())) {
                    std::cerr << "stream usage commit failed: " << request_id << '\n';
                }
            } catch (const std::exception &err) {
                std::cerr << "stream usage callback failed: " << err.what() << " request_id=" << request_id << '\n';
            }
        });
    set_stream_correlation_headers(res, request_id, response_id);
}

} // namespace revlm
