#pragma once

#include "config/config.hpp"
#include "server/http_server.hpp"
#include "version/version.hpp"

#include <httplib.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace revlm
{

void register_http_routes(::httplib::Server &server, const Config &config, const BuildInfo &build,
                          const std::shared_ptr<std::atomic_bool> &draining);

std::string inject_request_metadata(std::string_view request, std::string_view client_ip);

} // namespace revlm
