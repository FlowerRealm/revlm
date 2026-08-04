#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace revlm::plugin
{

// Host platform as seen by the package layout. The platform directory in a
// package is declared by convention: `backend/amd/` or `backend/arm/`. os is
// informational (darwin/linux); arch is the directory name and is the only
// value used to pick a module.
struct PluginPlatform {
    std::string os;
    std::string arch; // fixed set: "amd" | "arm"
};

// The v1 package manifest. plugin.json carries exactly these five fields and
// nothing else: no format_version, core_abi, requires, targets, load_order or
// migrations. `type` is the plugin category; "channel" is the only legal value.
struct PluginPackage {
    std::string id;
    std::string type;
    std::string name;
    std::string description;
    std::string version;
};

// A plugin selected for the next worker run. `root` is the single package
// directory (REVLM_PLUGIN_DIR/packages/<id>/ or the system equivalent), and
// `module` is the absolute path of the current platform's .so inside it.
struct ActivePlugin {
    PluginPackage package;
    std::filesystem::path root;
    std::filesystem::path module;
    bool system = false;
};

PluginPlatform current_plugin_platform();
bool plugin_identifier_is_safe(std::string_view value);
PluginPackage read_plugin_package(const std::filesystem::path &root);

// Returns the relative path of the single .so under backend/<arch>/ (e.g.
// "backend/amd/libX.so"), or nullopt when the platform directory is missing
// or does not contain exactly one .so.
std::optional<std::filesystem::path> module_path_for_platform(const std::filesystem::path &root,
                                                              const PluginPlatform &platform);

// Enumerate the active (enabled) plugin set from both roots and order it by
// plugin id lexicographically. User packages override system packages of the
// same id; enable state comes from filesystem markers under `plugin_dir`.
// Corrupt packages are absent from the set rather than taking the bootstrap
// down: install_plugin_archive validates before anything is published.
std::vector<ActivePlugin> active_plugins(const std::filesystem::path &plugin_dir,
                                         const std::filesystem::path &system_plugin_dir);

} // namespace revlm::plugin
