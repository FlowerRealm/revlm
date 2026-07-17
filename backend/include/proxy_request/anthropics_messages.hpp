#pragma once

#include <httplib.h>

#include <functional>
#include <string_view>

#include "proxy_request/api_orchestrate.hpp"
#include "proxy_request/openai_responses.hpp"

namespace revlm
{

ResponsesProxyResult run_messages_gateway(const ::httplib::Request &req, std::string_view request_id,
                                          long long channel_id);
void run_messages_stream(::httplib::Response &res, const ::httplib::Request &req, const GatewayParsedRequest &parsed,
                         std::string_view request_id, long long channel_id, std::string_view client_ip,
                         const std::function<void(ResponsesProxyResult)> &on_stream_usage);

} // namespace revlm
