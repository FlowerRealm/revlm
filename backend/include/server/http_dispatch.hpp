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

using V1Route = std::function<void(const ::httplib::Request &, ::httplib::Response &, ProxyRequest &)>;

void register_http_routes(::httplib::Server &server, const std::shared_ptr<std::atomic_bool> &draining);
extern "C" void revlm_register_http_routes(::httplib::Server &server,
                                           const std::shared_ptr<std::atomic_bool> &draining);

::httplib::Server::Handler v1_http(V1Route route);

std::string inject_request_metadata(std::string_view request, std::string_view client_ip);
ProxyRequest make_request(const ::httplib::Request &req, std::string_view request_id = {});
json data_plane_models_response(long long channel_group_id);
json data_plane_model_retrieve_response(std::string_view model_id, long long channel_group_id, bool &not_found);
void proxy_stream_commit_usage(ProxyRequest &pr);
void finish_proxy_usage(::httplib::Response &res, ProxyRequest &pr);

} // namespace revlm
