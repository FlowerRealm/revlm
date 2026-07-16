#pragma once

#include <httplib.h>
#include <string_view>

#include "proxy_request/api_orchestrate.hpp"
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

HttpResponse run_responses_compact_gateway(const ::httplib::Request &req, std::string_view request_id,
                                           long long usage_event_id);
void run_responses_compact_stream(::httplib::Response &res, const ::httplib::Request &req,
                                  const GatewayParsedRequest &parsed, std::string_view request_id,
                                  long long usage_event_id, std::string_view client_ip);

} // namespace revlm
