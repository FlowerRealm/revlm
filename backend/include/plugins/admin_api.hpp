#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "util/json.hpp"

namespace revlm::plugin
{

/* List installed plugins, admin only. data is PluginInfo[] (see host.hpp). */
json admin_plugins_response(std::string_view raw_request, std::string *set_cookie = nullptr);

/* Install (or replace) a package from an uploaded .revlm-plugin archive, admin only. */
json admin_plugin_upload_response(std::string_view raw_request, std::string_view archive,
                                  std::string *set_cookie = nullptr);

/* Enable or disable a plugin, admin only. Takes effect on the registry immediately. */
json admin_plugin_enable_response(std::string_view raw_request, std::string_view plugin_id, bool enabled,
                                  std::string *set_cookie = nullptr);

/* Mark a plugin for uninstall, admin only. The package is not deleted until
 * the next cold start runs its cleanup entry point. */
json admin_plugin_uninstall_response(std::string_view raw_request, std::string_view plugin_id,
                                     std::string *set_cookie = nullptr);

/*
 * Frontend entry manifest: one {id, url} row per installed package that has a
 * frontend/entry.js. No auth -- this is what the core frontend itself needs
 * in order to know what to import, not an admin surface.
 */
json plugin_frontend_entries_response();

/*
 * Resolve plugin_id + relative_path to an on-disk file strictly inside that
 * package's frontend/ directory. Empty on any validation failure: unknown
 * plugin id, missing file, or a path that would escape frontend/.
 */
std::optional<std::filesystem::path> plugin_frontend_asset(std::string_view plugin_id, std::string_view relative_path);

} // namespace revlm::plugin
