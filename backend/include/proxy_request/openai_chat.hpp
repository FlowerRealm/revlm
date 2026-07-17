#pragma once

#include <httplib.h>

#include <functional>
#include <string_view>

#include "proxy_request/api_orchestrate.hpp"
#include "request/request.hpp"
#include "server/http_server.hpp"

namespace revlm
{

HttpResponse run_chat_completions_gateway(const ::httplib::Request &req, std::string_view request_id,
                                          long long channel_id, Request &usage);
void run_chat_completions_stream(::httplib::Response &res, const ::httplib::Request &req,
                                 const GatewayParsedRequest &parsed, std::string_view request_id, long long channel_id,
                                 std::string_view client_ip, Request usage,
                                 const std::function<void(Request &)> &on_usage);

} // namespace revlm
