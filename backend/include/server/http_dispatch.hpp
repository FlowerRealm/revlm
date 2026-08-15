#pragma once

#include <httplib.h>

#include <atomic>
#include <functional>
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

/*
 * The prefix-free proxy catch-all. Kept separate from register_http_routes
 * because it must be registered after the plugins are loaded: httplib matches in
 * registration order, so core routes must come first (a plugin must not be able
 * to shadow /api/user/login) and plugin endpoints must come before this (or this
 * swallows them).
 */
void register_proxy_catch_all(::httplib::Server &server);

std::string inject_request_metadata(std::string_view request, std::string_view client_ip);
ProxyRequest make_request(const ::httplib::Request &req, std::string_view request_id = {});

} // namespace revlm
