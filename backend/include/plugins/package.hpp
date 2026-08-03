#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace revlm::plugin
{

// This stamp describes the public factory/registrar ABI. It is intentionally
// separate from the host's private C++ implementation details.
inline constexpr std::string_view k_sdk_abi = "revlm-plugin-cpp-v1";

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
    std::string sdk_abi;
    std::vector<std::string> dependencies;
    std::vector<PluginModule> modules;
    std::string frontend_schema;
    std::vector<std::string> migrations;
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

// Resolve the exact package roots selected for the next worker. No module code
// is loaded here; the worker owns the v1 dlopen lifecycle.
std::vector<ActivePlugin> active_plugins(const std::filesystem::path &plugin_dir,
                                         const std::filesystem::path &system_plugin_dir);

} // namespace revlm::plugin
