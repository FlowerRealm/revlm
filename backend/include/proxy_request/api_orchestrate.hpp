#pragma once

#include <httplib.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "models/models.hpp"
#include "proxy_request/gateway_resilience.hpp"
#include "proxy_request/routing_data_source.hpp"
#include "proxy_request/upstream.hpp"
#include "request/request.hpp"
#include "scheduler/scheduler.hpp"
#include "server/http_server.hpp"

namespace revlm
{

struct GatewayParsedRequest {
    std::string_view method;
    std::string_view path;
    std::string_view target;
    size_t header_bytes = 0;
    size_t content_length = 0;
    bool invalid_framing = false;
};

struct ProxyGatewayContext {
    ProxyRoutingDataSource data_source;
    Scheduler scheduler;

    ProxyGatewayContext()
        : data_source()
        , scheduler(data_source)
    {
    }
};

struct ScheduledUpstreamExecution {
    std::optional<UpstreamExecutionResult> result;
    std::optional<GatewayAttemptTransportError> transport_error;
};

struct ScheduledUpstreamStreamExecution {
    std::optional<UpstreamStreamResponse> result;
    std::optional<GatewayAttemptTransportError> transport_error;
};

void apply_http_response(const HttpResponse &response, ::httplib::Response &res);

std::string upstream_response_id_from_headers(const std::vector<UpstreamHeader> &headers);
void assign_request_correlation(Request &request, std::string_view request_id, std::string_view response_id);
void set_stream_correlation_headers(::httplib::Response &res, std::string_view request_id,
                                    std::string_view response_id);
std::vector<Header> merge_correlation_headers(const std::vector<UpstreamHeader> &upstream_headers,
                                              std::string_view request_id, std::string_view response_id);

const Model *billing_model_for_name(std::string_view name);
std::optional<HttpResponse> paygo_balance_gate(long long user_id, std::string_view request_id);

bool commit_proxy_usage(Request &usage_request);

SchedulerConstraints build_scheduler_constraints(long long channel_id, std::string_view requested_model,
                                                 SchedulerApi required_api);
SchedulerResult scheduler_result_from_upstream_status(int status_code);
void report_upstream_status(Scheduler &scheduler, const SchedulerSelection &selection, int status_code);
void report_upstream_transport_failure(Scheduler &scheduler, const SchedulerSelection &selection,
                                       const GatewayAttemptTransportError &transport_error);

ScheduledUpstreamExecution execute_scheduled_upstream(const SchedulerSelection &selection, UpstreamRequest downstream);
ScheduledUpstreamStreamExecution open_scheduled_upstream_stream(const SchedulerSelection &selection,
                                                                UpstreamRequest downstream);

std::string replace_json_string_field(std::string_view json, std::string_view field_name, std::string_view replacement);
std::string remove_json_field(std::string_view json, std::string_view field_name);

std::vector<UpstreamHeader> proxy_forward_headers(const ::httplib::Request &req, std::string_view request_id,
                                                  std::string_view client_ip,
                                                  std::function<bool(std::string_view)> drop_header = {});
UpstreamRequest build_proxy_upstream_request(const ::httplib::Request &req, std::string_view path,
                                             std::string_view request_id, std::string_view client_ip, std::string body,
                                             std::function<bool(std::string_view)> drop_header = {});
} // namespace revlm
