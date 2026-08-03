#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "plugins/package.hpp"
#include "plugins/sdk.hpp"

namespace revlm::plugin
{

// Load the worker's immutable package snapshot once. Handles and plugin
// objects remain alive until shutdown; V1 has no hot unload.
void load_plugins_for_worker(const std::vector<ActivePlugin> &plugins);

void register_data_plane_routes(::httplib::Server &server);

std::vector<Model> models_for_channel_type(std::string_view channel_type);
std::vector<Model> all_plugin_models();
void prepare_upstream_for_channel(const Channel &channel, const UpstreamRequest &downstream,
                                  UpstreamPreparedRequest &prepared);
bool retry_upstream_for_channel(const Channel &channel, const UpstreamPreparedRequest &prepared,
                                const UpstreamResponse &response, UpstreamPreparedRequest &retry);

json plugin_channel_types_json();

} // namespace revlm::plugin
