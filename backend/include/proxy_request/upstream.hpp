#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include "auth/security.hpp"
#include "scheduler/scheduler.hpp"

namespace revlm
{

struct UpstreamHeader {
    std::string name;
    std::string value;
};

struct UpstreamRequest {
    std::string method = "POST";
    std::string path = "/v1/responses";
    std::string query;
    std::vector<UpstreamHeader> headers;
    std::string body;
};

struct UpstreamPreparedRequest {
    SchedulerSelection selection;
    ValidatedBaseUrl base_url;
    std::string method = "POST";
    std::string url;
    std::vector<UpstreamHeader> headers;
    std::string body;
    bool retried_unsupported_parameter = false;
};

struct UpstreamResponse {
    int status_code = 0;
    std::vector<UpstreamHeader> headers;
    std::string body;
};

struct UpstreamExecutionResult {
    UpstreamPreparedRequest request;
    UpstreamResponse response;
    bool rewrote_unsupported_parameter = false;
};

struct UpstreamReadHandle {
    std::function<ssize_t(char *, size_t)> read;
    std::function<void()> close;
    int poll_fd = -1;
};

struct UpstreamStreamResponse {
    UpstreamPreparedRequest request;
    int status_code = 0;
    std::vector<UpstreamHeader> headers;
    std::string initial_body;
    UpstreamReadHandle stream;
};

using UpstreamTransport = std::function<UpstreamResponse(const UpstreamPreparedRequest &)>;

class UpstreamExecutor {
public:
    UpstreamPreparedRequest prepare(const SchedulerSelection &selection, UpstreamRequest downstream,
                                    bool retried_unsupported_parameter = false, bool enforce_ssrf = true) const;
    UpstreamExecutionResult execute(const SchedulerSelection &selection, UpstreamRequest downstream,
                                    const UpstreamTransport &transport, bool enforce_ssrf = true) const;
};

UpstreamPreparedRequest rewrite_for_unsupported_parameter_retry(const UpstreamPreparedRequest &prepared,
                                                                const UpstreamResponse &response);
std::string build_upstream_url(const ValidatedBaseUrl &base_url, std::string_view downstream_path,
                               std::string_view query);
UpstreamResponse default_upstream_http_transport(const UpstreamPreparedRequest &prepared, int timeout_ms = 30000,
                                                 bool allow_private_target = false);
UpstreamStreamResponse default_upstream_http_stream_transport(const UpstreamPreparedRequest &prepared,
                                                              int timeout_ms = 30000,
                                                              bool allow_private_target = false);

UpstreamTransport make_default_upstream_transport(int timeout_ms, bool allow_private_target = false);
UpstreamExecutionResult execute_with_default_transport(const UpstreamExecutor &executor,
                                                       const SchedulerSelection &selection, UpstreamRequest downstream,
                                                       int timeout_ms, bool allow_private_target = false);
bool upstream_channel_allows_private_target(std::string_view base_url);
bool is_hop_by_hop_header(std::string_view name);

} // namespace revlm
