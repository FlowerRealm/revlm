#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace revlm::plugin
{

// This is a compatibility stamp, not an extension API. A preload module uses
// the core's real C++ symbols and therefore must be built for this exact ABI.
inline constexpr std::string_view k_core_abi = "revlm-core-preload-v2";

struct PluginPlatform {
    std::string os;
    std::string arch;
};

struct PluginModule {
    std::string os;
    std::string arch;
    std::string path;
};

struct PluginPackage {
    std::string id;
    std::string name;
    std::string version;
    std::string core_abi;
    std::vector<std::string> dependencies;
    std::vector<PluginModule> modules;
    std::vector<std::string> migrations;
    int load_order = 0;
};

struct ActivePlugin {
    PluginPackage package;
    std::filesystem::path root;
    std::filesystem::path module;
    bool system = false;
};

PluginPlatform current_plugin_platform();
bool plugin_identifier_is_safe(std::string_view value);
PluginPackage read_plugin_package(const std::filesystem::path &root);
const PluginModule *module_for_platform(const PluginPackage &package, const PluginPlatform &platform);

// Resolve the exact modules that the next worker process will preload. This
// function deliberately knows nothing about routes, channel types, handlers,
// or frontend schemas.
std::vector<ActivePlugin> active_plugins(const std::filesystem::path &plugin_dir,
                                         const std::filesystem::path &system_plugin_dir);

} // namespace revlm::plugin
