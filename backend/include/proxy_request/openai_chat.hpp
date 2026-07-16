#pragma once

#include <httplib.h>
#include <string_view>

#include "proxy_request/api_orchestrate.hpp"

namespace revlm
{

HttpResponse run_chat_completions_gateway(const ::httplib::Request &req, std::string_view request_id,
                                          long long usage_event_id);
void run_chat_completions_stream(::httplib::Response &res, const ::httplib::Request &req,
                                 const GatewayParsedRequest &parsed, std::string_view request_id,
                                 long long usage_event_id, std::string_view client_ip);

} // namespace revlm
