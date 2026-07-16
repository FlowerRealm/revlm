#pragma once

#include <httplib.h>
#include <string_view>

#include "proxy_response/upstream_http.hpp"
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
};

ResponsesProxyResult handle_responses_proxy_request(std::string_view raw_request, std::string_view method,
                                                    std::string_view path, std::string_view request_id,
                                                    long long usage_event_id,
                                                    const ResponsesProxyExecuteOptions &options = {});

} // namespace revlm
