#pragma once

#include <httplib.h>
#include <string_view>

#include "config/config.hpp"
#include "server/http_server.hpp"

namespace revlm
{

struct GatewayParsedRequest {
    std::string_view method;
    std::string_view path;
    std::string_view target;
    size_t header_bytes = 0;
    size_t content_length = 0;
    bool invalid_framing = false;
};

HttpResponse run_chat_completions_gateway(const ::httplib::Request &req, const Config &config,
                                          std::string_view request_id, long long usage_event_id);
void run_chat_completions_stream(::httplib::Response &res, const ::httplib::Request &req,
                                 const GatewayParsedRequest &parsed, const Config &config, std::string_view request_id,
                                 long long usage_event_id, std::string_view client_ip);

HttpResponse run_messages_gateway(const ::httplib::Request &req, const Config &config, std::string_view request_id,
                                  long long usage_event_id);
void run_messages_stream(::httplib::Response &res, const ::httplib::Request &req, const GatewayParsedRequest &parsed,
                         const Config &config, std::string_view request_id, long long usage_event_id,
                         std::string_view client_ip);

HttpResponse run_responses_compact_gateway(const ::httplib::Request &req, const Config &config,
                                           std::string_view request_id, long long usage_event_id);
void run_responses_compact_stream(::httplib::Response &res, const ::httplib::Request &req,
                                  const GatewayParsedRequest &parsed, const Config &config, std::string_view request_id,
                                  long long usage_event_id, std::string_view client_ip);

void apply_http_response(const HttpResponse &response, ::httplib::Response &res);

} // namespace revlm
