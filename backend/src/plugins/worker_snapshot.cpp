#include "plugins/packages.hpp"

#include "config/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace revlm::plugin
{
namespace
{

namespace fs = std::filesystem;

std::vector<fs::path> snapshot_roots()
{
    const char *raw = std::getenv("REVLM_PLUGIN_ROOTS");
    if (raw == nullptr || *raw == '\0') {
        return {};
    }
    std::vector<fs::path> roots;
    std::string_view remaining{ raw };
    while (!remaining.empty()) {
        const std::size_t newline = remaining.find('\n');
        const std::string_view row = remaining.substr(0, newline);
        const std::size_t tab = row.find('\t');
        if (tab != std::string_view::npos) {
            const std::string id{ row.substr(0, tab) };
            const fs::path root{ row.substr(tab + 1) };
            if (plugin_identifier_is_safe(id) && !root.empty()) {
                roots.push_back(root);
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(newline + 1);
    }
    return roots;
}

} // namespace

std::vector<ActivePlugin> worker_plugin_snapshot()
{
    const PluginPlatform platform = current_plugin_platform();
    std::vector<ActivePlugin> out;
    for (const fs::path &root : snapshot_roots()) {
        try {
            const PluginPackage package = read_plugin_package(root);
            const PluginModule *module = module_for_platform(package, platform);
            if (module == nullptr || !fs::is_regular_file(root / module->path)) {
                throw std::runtime_error("plugin has no current-platform module");
            }
            out.push_back({ package, root, root / module->path, false });
        } catch (const std::exception &error) {
            throw std::runtime_error("invalid worker plugin snapshot: " + std::string{ error.what() });
        }
    }
    return out;
}

} // namespace revlm::plugin
