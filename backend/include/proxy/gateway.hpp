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

#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "proxy/upstream.hpp"
#include "request/proxy_request.hpp"
#include "util/json.hpp"

namespace revlm
{

using ClientWriter = std::function<bool(std::string_view)>;

// Internal v2-era transport/billing helpers kept as the core's own
// implementation detail. Not part of the plugin ABI: v3 plugins enter through
// the extern "C" hooks below (revlm_handle_v1 / revlm_next_candidate /
// revlm_commit_request) and reuse the ordinary upstream transport.
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
json make_proxy_error(int status, json error_body);

std::string upstream_response_id_from_headers(const std::vector<UpstreamHeader> &headers);
void assign_request_correlation(ProxyRequest &pr, std::string_view response_id);
void set_stream_correlation_headers(::httplib::Response &res, std::string_view response_id);
std::vector<UpstreamHeader> merge_correlation_headers(const std::vector<UpstreamHeader> &upstream_headers,
                                                      std::string_view response_id);

std::optional<json> paygo_balance_gate(long long user_id);

bool commit_proxy_usage(ProxyRequest &pr);

ScheduledUpstreamExecution execute_scheduled_upstream(long long channel_id, UpstreamRequest downstream);
ScheduledUpstreamStreamExecution open_scheduled_upstream_stream(long long channel_id, UpstreamRequest downstream);

std::string remove_json_field(std::string_view json, std::string_view field_name);

UpstreamRequest build_proxy_upstream_request(const ProxyRequest &pr, std::string_view path);

ClientWriter client_writer_from_fd(int fd);

bool is_sse_content_type(std::string_view content_type);

std::string drain_upstream_stream_body(UpstreamStreamResponse &upstream);

std::string format_upstream_proxy_response_headers(int status_code, const std::vector<UpstreamHeader> &headers,
                                                   size_t body_size);

std::string build_synthetic_stream_response_head(int status, std::string_view content_type,
                                                 const std::vector<UpstreamHeader> &headers = {});

std::string read_remaining_stream(const UpstreamReadHandle &stream);

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

struct GatewayStreamResult {
    GatewayStreamPump pump;
};

// Reusable upstream-to-client stream relay (the "SSE pump"). The plugin owns
// protocol parsing: it supplies an optional on_chunk callback to scan SSE
// events / usage as bytes arrive, and decides completion itself. The core
// pump only relays bytes and reports transport-level state (disconnect,
// idle timeout, upstream error). It keeps draining after client disconnect so
// the plugin can still observe the final usage events.
using StreamChunkHandler = std::function<void(std::string_view, const GatewayStreamPump &)>;
GatewayStreamResult pump_upstream_stream(const std::function<ssize_t(char *, size_t)> &read_chunk,
                                         const std::function<bool(std::string_view)> &write_to_client,
                                         std::string_view initial_body, int idle_timeout_ms, int poll_fd,
                                         const StreamChunkHandler &on_chunk = {});

// -- v3 plugin data-plane ABI (ADR-0003) ------------------------------------
//
// The plugin hooks are the single /v1 entry (revlm_handle_v1) and two core
// ordinary functions the plugin calls during one hook invocation
// (revlm_next_candidate, revlm_commit_request). These are dynamic-link
// interposable symbols; a plugin provides the same C name and is reached
// through LD_PRELOAD/DYLD_INSERT_LIBRARIES. The core's own definitions below
// are the fallback / RTLD_NEXT target and the no-plugin 500 path.

// Core default /v1 handler (also the RTLD_NEXT tail of the plugin chain).
// Exposed so the preload chain can reach it via dlsym(RTLD_NEXT) or so a
// no-plugin build still links; the route installs it through the HTTP layer.
extern "C" void revlm_handle_v1(const ::httplib::Request &req, ::httplib::Response &res, ProxyRequest &proxy);

// Core candidate-iteration helper for the plugin hook: advances the
// round-robin pointer and returns the next active candidate Channel.
extern "C" const Channel *revlm_next_candidate(ProxyRequest &proxy);

// Core final-commit helper for the plugin hook: applies the ChannelGroup
// multiplier, debits, and persists the core request record.
extern "C" bool revlm_commit_request(ProxyRequest &proxy);

} // namespace revlm
