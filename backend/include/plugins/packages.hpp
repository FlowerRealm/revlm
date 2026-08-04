#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "plugins/package.hpp"
#include "util/json.hpp"

namespace revlm::plugin
{

struct PluginActionResult {
    bool ok = false;
    std::string message;
};

// Package handling is intentionally boring: it owns archive extraction,
// durable filesystem enable state and lifecycle symbol calls. It never loads a
// module into the running worker and never asks one what it is allowed to do.
PluginActionResult install_plugin_archive(std::string_view archive_bytes);
PluginActionResult set_plugin_enabled(std::string_view plugin_id, bool enabled);
PluginActionResult schedule_plugin_uninstall(std::string_view plugin_id);

// Called by the short-lived bootstrap before it execs the real worker: pending
// uninstalls run their cleanup first, then every enabled plugin runs its
// migrate symbol, then the returned modules (id-lexicographic) are placed into
// LD_PRELOAD by the caller. Any migrate failure throws, which refuses to start
// the worker.
std::vector<ActivePlugin> prepare_plugins_for_worker();

// Frontend assets are the package snapshot captured at worker bootstrap via
// REVLM_PRELOADED_PLUGIN_ROOTS; uploads and enable changes never mutate the
// running worker's asset list.
json plugin_frontend_entries_json();
std::optional<std::filesystem::path> plugin_frontend_file(std::string_view plugin_id, std::string_view relative_path);

} // namespace revlm::plugin
