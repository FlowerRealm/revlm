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

struct PluginInstallation {
    std::string id;
    std::string version;
    std::string display_name;
    std::string core_abi;
    std::string status;
    std::string package_path;
    std::string target_os;
    std::string target_arch;
    std::string error_message;
    bool enabled = true;
    bool system_plugin = false;
};

struct PluginActionResult {
    bool ok = false;
    std::string message;
};

// Package handling is intentionally boring: it owns archive extraction,
// durable enable state, and migrations. It never asks a module what it is
// allowed to replace and never loads one into the running worker.
PluginActionResult install_plugin_archive(std::string_view archive_bytes);
PluginActionResult set_plugin_enabled(std::string_view plugin_id, bool enabled);
PluginActionResult schedule_plugin_uninstall(std::string_view plugin_id);
json plugin_installations_json();

// Called by the short-lived bootstrap before it execs the real worker. SQL
// migrations finish here, then the returned modules are placed in LD_PRELOAD.
std::vector<ActivePlugin> prepare_plugins_for_worker();

json plugin_frontend_entries_json();
std::optional<std::filesystem::path> plugin_frontend_file(std::string_view plugin_id, std::string_view relative_path);

} // namespace revlm::plugin
