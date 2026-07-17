#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <httplib.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include <boost/json/object.hpp>

#include "models/models.hpp"
#include "proxy/upstream.hpp"
#include "request/request.hpp"
#include "server/http_server.hpp"

namespace revlm
{

class Gateway {
public:
    Gateway(Request &usage, const Model *model, double tier_multiplier, double channel_multiplier)
        : request(usage)
    {
        if (model != nullptr) {
            request.pricing_model = model;
            if (request.model_name.null() || request.model_name->empty()) {
                request.model_name = model->name;
            }
        }
        request.tier_multiplier = tier_multiplier;
        request.channel_multiplier = channel_multiplier;
    }
    virtual ~Gateway() = default;
    virtual void finalize(boost::json::object &json) = 0;

protected:
    Request &request;
};

struct GatewayStreamPump {
    bool completed = false;
    bool client_disconnected = false;
    bool idle_timeout = false;
    bool upstream_error = false;
    bool saw_usage = false;
    size_t response_bytes = 0;
    int first_token_latency_ms = 0;
    std::optional<std::string> model;
};

struct GatewayRetryPolicy {
    int max_attempts = 1;
    int max_switches = 0;
    int max_elapsed_ms = 0;
};

class GatewayRetryBudget {
public:
    explicit GatewayRetryBudget(GatewayRetryPolicy policy);

    bool can_attempt(bool switching) const;
    void note_attempt(bool switched);

    int attempts() const
    {
        return attempts_;
    }
    int switches() const
    {
        return switches_;
    }
    int elapsed_ms() const;

private:
    GatewayRetryPolicy policy_;
    std::chrono::steady_clock::time_point started_at_;
    int attempts_ = 0;
    int switches_ = 0;
};

struct GatewayFailure {
    bool retriable = false;
    bool preserve_upstream_response = false;
    int status_code = 502;
    std::string error_class;
    std::string error_message;
};

struct GatewayAttemptTransportError {
    std::string stage;
    std::string message;
};

GatewayFailure classify_gateway_status_failure(int status_code);
GatewayFailure classify_gateway_transport_failure(std::string_view stage, std::string_view message = {});
GatewayFailure classify_gateway_stream_failure(const GatewayStreamPump &pump, int upstream_status_code);
size_t best_gateway_failure_index(const std::vector<GatewayFailure> &failures);

struct GatewayParsedRequest {
    std::string_view method;
    std::string_view path;
    std::string_view target;
    size_t header_bytes = 0;
    size_t content_length = 0;
    bool invalid_framing = false;
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

ScheduledUpstreamExecution execute_scheduled_upstream(long long channel_id, UpstreamRequest downstream);
ScheduledUpstreamStreamExecution open_scheduled_upstream_stream(long long channel_id, UpstreamRequest downstream);

std::string remove_json_field(std::string_view json, std::string_view field_name);

std::vector<UpstreamHeader> proxy_forward_headers(const ::httplib::Request &req, std::string_view request_id,
                                                  std::string_view client_ip,
                                                  std::function<bool(std::string_view)> drop_header = {});
UpstreamRequest build_proxy_upstream_request(const ::httplib::Request &req, std::string_view path,
                                             std::string_view request_id, std::string_view client_ip, std::string body,
                                             std::function<bool(std::string_view)> drop_header = {});

using ClientWriter = std::function<bool(std::string_view)>;

ClientWriter client_writer_from_fd(int fd);

bool is_sse_content_type(std::string_view content_type);

HttpResponse make_upstream_http_response(int status, std::string body, std::vector<Header> headers = {});

std::string drain_upstream_stream_body(UpstreamStreamResponse &upstream);

std::string format_upstream_proxy_response_headers(int status_code, const std::vector<UpstreamHeader> &headers,
                                                   size_t body_size);

std::string build_synthetic_stream_response_head(int status, std::string_view content_type,
                                                 const std::vector<Header> &headers = {});

std::string read_remaining_stream(const UpstreamReadHandle &stream);

enum class GatewayStreamKind {
    openai_chat,
    openai_responses,
    anthropics_messages,
};

struct GatewayStreamResult {
    GatewayStreamPump pump;
};

std::unique_ptr<Gateway> make_gateway(GatewayStreamKind kind, const Model *model, double tier_multiplier,
                                      double channel_multiplier, Request &usage);

void parse_billing_request_from_body(Request &out, GatewayStreamKind kind, std::string_view body);

GatewayStreamResult pump_gateway_stream(const std::function<ssize_t(char *, size_t)> &read_chunk,
                                        const std::function<bool(std::string_view)> &write_to_client,
                                        std::string_view initial_body, int idle_timeout_ms, int poll_fd,
                                        Gateway &gateway);

void apply_upstream_gateway_stream(::httplib::Response &res, int status, const std::vector<UpstreamHeader> &headers,
                                   UpstreamStreamResponse upstream, Request usage,
                                   std::function<std::unique_ptr<Gateway>(Request &)> make_gateway_for_usage,
                                   std::string_view requested_service_tier,
                                   std::function<void(Request &usage, const GatewayStreamResult &)> on_complete = {});

} // namespace revlm
