#pragma once

#include <httplib.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "proxy_response/upstream_http.hpp"
#include "request/request.hpp"
#include "server/http_server.hpp"

namespace revlm
{

struct ResponsesProxyExecuteOptions {
    int client_fd = -1;
    ClientWriter write_client;
    ::httplib::Response *stream_response = nullptr;
};

struct ResponsesProxyResult {
    HttpResponse response;
    bool handled_stream = false;
    int stream_status = 0;

    // Usage materials for http_dispatch billing (proxy does not commit).
    bool has_usage = false;
    bool billable = false;
    std::string forwarded_model;
    long long channel_id = 0;
    int status_code = 0;
    std::string response_body;
    double channel_multiplier = 1.0;
    std::string response_id;
    bool is_stream = false;
    std::string error_class;
    std::string error_message;
    std::string service_tier;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    std::optional<Request> billing_request;
};

ResponsesProxyResult
handle_responses_proxy_request(const ::httplib::Request &req, std::string_view method, std::string_view path,
                               std::string_view request_id, long long channel_id,
                               const ResponsesProxyExecuteOptions &options = {},
                               const std::function<void(ResponsesProxyResult)> &on_stream_usage = {});

} // namespace revlm
