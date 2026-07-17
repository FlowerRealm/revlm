#pragma once

#include "proxy_request/upstream.hpp"
#include "proxy_response/gateway.hpp"
#include "proxy_response/gateway_stream.hpp"
#include "request/request.hpp"

#include <httplib.h>

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace revlm
{

enum class GatewayStreamKind {
    openai_chat,
    openai_responses,
    anthropics_messages,
};

struct GatewayStreamResult {
    GatewayStreamPump pump;
};

std::unique_ptr<Gateway> make_gateway(GatewayStreamKind kind, const Model *model, double tier_multiplier,
                                      double channel_multiplier, Request &usage);

void parse_billing_request_from_body(Request &out, GatewayStreamKind kind, std::string_view body);

GatewayStreamResult pump_gateway_stream(const std::function<ssize_t(char *, size_t)> &read_chunk,
                                        const std::function<bool(std::string_view)> &write_to_client,
                                        std::string_view initial_body, int idle_timeout_ms, int poll_fd,
                                        Gateway &gateway);

void apply_upstream_gateway_stream(::httplib::Response &res, int status, const std::vector<UpstreamHeader> &headers,
                                   UpstreamStreamResponse upstream, Request usage,
                                   std::function<std::unique_ptr<Gateway>(Request &)> make_gateway_for_usage,
                                   std::string_view requested_service_tier,
                                   std::function<void(Request &usage, const GatewayStreamResult &)> on_complete = {});

} // namespace revlm
