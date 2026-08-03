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
    std::string core_abi; // legacy API/storage name; contains the v1 sdk_abi value
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

// Called by the bootstrap before exec; it validates package state and returns
// the exact roots that the worker will load through the V1 SDK runtime.
std::vector<ActivePlugin> prepare_plugins_for_worker();
std::vector<ActivePlugin> worker_plugin_snapshot();
void apply_plugin_migrations(const ActivePlugin &plugin);
void set_plugin_runtime_state(std::string_view plugin_id, std::string_view status, std::string_view message);

} // namespace revlm::plugin
