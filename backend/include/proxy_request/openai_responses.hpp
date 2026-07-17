#pragma once

#include <httplib.h>
#include <functional>
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
    std::function<void(Request &)> on_usage; // Path B only; Path C must be empty
};

struct ResponsesProxyResult {
    HttpResponse response;
    bool handled_stream = false;
    int stream_status = 0;
};

ResponsesProxyResult handle_responses_proxy_request(const ::httplib::Request &req, std::string_view method,
                                                    std::string_view path, std::string_view request_id,
                                                    long long channel_id, Request &usage,
                                                    const ResponsesProxyExecuteOptions &options = {});

} // namespace revlm
