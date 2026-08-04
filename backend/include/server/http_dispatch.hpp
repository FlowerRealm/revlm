#pragma once

#include <httplib.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "request/proxy_request.hpp"
#include "util/json.hpp"

namespace revlm
{

void register_http_routes(::httplib::Server &server, const std::shared_ptr<std::atomic_bool> &draining);
extern "C" void revlm_register_http_routes(::httplib::Server &server,
                                           const std::shared_ptr<std::atomic_bool> &draining);

// Core-owned single /v1 data-plane entry (ADR-0003). Authenticates the API
// key, resolves the ChannelGroup snapshot into the ProxyRequest, then calls
// the interposable revlm_handle_v1 hook (plugin-provided or core fallback).
::httplib::Server::Handler make_v1_handler();

std::string inject_request_metadata(std::string_view request, std::string_view client_ip);
ProxyRequest make_request(const ::httplib::Request &req, std::string_view request_id = {});

} // namespace revlm
