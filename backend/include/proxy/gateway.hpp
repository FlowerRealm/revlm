#pragma once

#include <cstddef>
#include <functional>
#include <httplib.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include "request/proxy_request.hpp"
#include "util/json.hpp"
#include "proxy/upstream.hpp"
#include "request/request.hpp"

namespace revlm
{

class Gateway {
public:
    Gateway(ProxyRequest &pr)
        : request(pr)
    {
    }
    virtual ~Gateway() = default;
    virtual void finalize(json &json) = 0;

protected:
    ProxyRequest &request;
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

struct GatewayAttemptTransportError {
    std::string stage;
    std::string message;
};

struct ScheduledUpstreamExecution {
    std::optional<UpstreamExecutionResult> result;
    std::optional<GatewayAttemptTransportError> transport_error;
};

struct ScheduledUpstreamStreamExecution {
    std::optional<UpstreamStreamResponse> result;
    std::optional<GatewayAttemptTransportError> transport_error;
};

// Proxy exit: pass upstream status/body/headers through to the client socket.
void write_upstream(::httplib::Response &res, int status, std::string body,
                    const std::vector<UpstreamHeader> &headers = {});
void write_proxy_result(::httplib::Response &res, const json &result);

json headers_to_json(const std::vector<UpstreamHeader> &headers);
std::vector<UpstreamHeader> headers_from_json(const json &header_obj);
json make_proxy_result(int status, std::string body, const std::vector<UpstreamHeader> &headers = {});
json make_proxy_error(int status, std::string_view request_id, json error_body);

std::string upstream_response_id_from_headers(const std::vector<UpstreamHeader> &headers);
void assign_request_correlation(ProxyRequest &pr, std::string_view request_id, std::string_view response_id);
void set_stream_correlation_headers(::httplib::Response &res, std::string_view request_id,
                                    std::string_view response_id);
std::vector<UpstreamHeader> merge_correlation_headers(const std::vector<UpstreamHeader> &upstream_headers,
                                                      std::string_view request_id, std::string_view response_id);

std::optional<json> paygo_balance_gate(long long user_id);

bool commit_proxy_usage(ProxyRequest &pr);

ScheduledUpstreamExecution execute_scheduled_upstream(long long channel_id, UpstreamRequest downstream);
ScheduledUpstreamStreamExecution open_scheduled_upstream_stream(long long channel_id, UpstreamRequest downstream);

std::string remove_json_field(std::string_view json, std::string_view field_name);

UpstreamRequest build_proxy_upstream_request(const ProxyRequest &pr, std::string_view path);

using ClientWriter = std::function<bool(std::string_view)>;

ClientWriter client_writer_from_fd(int fd);

bool is_sse_content_type(std::string_view content_type);

std::string drain_upstream_stream_body(UpstreamStreamResponse &upstream);

std::string format_upstream_proxy_response_headers(int status_code, const std::vector<UpstreamHeader> &headers,
                                                   size_t body_size);

std::string build_synthetic_stream_response_head(int status, std::string_view content_type,
                                                 const std::vector<UpstreamHeader> &headers = {});

std::string read_remaining_stream(const UpstreamReadHandle &stream);

enum class GatewayStreamKind {
    openai_chat,
    openai_responses,
    anthropics_messages,
};

struct GatewayStreamResult {
    GatewayStreamPump pump;
};

std::unique_ptr<Gateway> make_gateway(GatewayStreamKind kind, ProxyRequest &pr);

void parse_billing_request_from_body(ProxyRequest &pr, GatewayStreamKind kind, std::string_view body);

GatewayStreamResult pump_gateway_stream(const std::function<ssize_t(char *, size_t)> &read_chunk,
                                        const std::function<bool(std::string_view)> &write_to_client,
                                        std::string_view initial_body, int idle_timeout_ms, int poll_fd,
                                        Gateway &gateway);

void apply_upstream_gateway_stream(
    ::httplib::Response &res, int status, const std::vector<UpstreamHeader> &headers, UpstreamStreamResponse upstream,
    ProxyRequest usage, std::function<std::unique_ptr<Gateway>(ProxyRequest &)> make_gateway_for_usage,
    std::string_view requested_service_tier,
    std::function<void(ProxyRequest &usage, const GatewayStreamResult &)> on_complete = {});

} // namespace revlm
