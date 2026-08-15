#include "plugins/admin_api.hpp"

#include <string>
#include <string_view>
#include <system_error>

#include "plugins/host.hpp"
#include "plugins/scan.hpp"
#include "users/user_api.hpp"

namespace revlm::plugin
{
namespace
{

namespace fs = std::filesystem;

bool require_root(std::string_view raw_request, json &error, std::string *set_cookie)
{
    return api_authenticated_admin(raw_request, error, set_cookie).has_value();
}

/*
 * Same shape as install.cpp's safe_entry_path: reject anything that could
 * step outside frontend/ before it ever touches the filesystem. A URL path
 * segment is untrusted request input, unlike an archive member that was
 * already checked once at install time.
 */
bool relative_path_is_safe(std::string_view raw)
{
    if (raw.empty() || raw.front() == '/' || raw.find('\\') != std::string_view::npos ||
        raw.find('\0') != std::string_view::npos) {
        return false;
    }
    const fs::path path{ raw };
    if (path.is_absolute()) {
        return false;
    }
    for (const fs::path &segment : path) {
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
    }
    return true;
}

// Belt-and-suspenders alongside relative_path_is_safe: catches an escape via
// a symlink planted inside the package that the segment check cannot see.
bool path_is_within(const fs::path &root, const fs::path &candidate)
{
    std::error_code error;
    const fs::path normalized_root = fs::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const fs::path normalized_candidate = fs::weakly_canonical(candidate, error);
    if (error) {
        return false;
    }
    auto root_it = normalized_root.begin();
    auto candidate_it = normalized_candidate.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == normalized_candidate.end() || *root_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

std::optional<fs::path> package_dir_for_id(std::string_view plugin_id)
{
    for (const InstalledPackage &package : all_installed_packages()) {
        if (package.id == plugin_id) {
            return package.dir;
        }
    }
    return std::nullopt;
}

} // namespace

json admin_plugins_response(std::string_view raw_request, std::string *set_cookie)
{
    json error;
    if (!require_root(raw_request, error, set_cookie)) {
        return error;
    }
    return json({ { "success", true }, { "data", to_json(plugin_infos()) } });
}

json admin_plugin_upload_response(std::string_view raw_request, std::string_view archive, std::string *set_cookie)
{
    json error;
    if (!require_root(raw_request, error, set_cookie)) {
        return error;
    }
    std::string message;
    if (!install_plugin_archive(archive, message)) {
        return json({ { "success", false }, { "message", message } });
    }
    return json({ { "success", true } });
}

json admin_plugin_enable_response(std::string_view raw_request, std::string_view plugin_id, bool enabled,
                                  std::string *set_cookie)
{
    json error;
    if (!require_root(raw_request, error, set_cookie)) {
        return error;
    }
    std::string message;
    if (!set_plugin_enabled(plugin_id, enabled, message)) {
        return json({ { "success", false }, { "message", message } });
    }
    return json({ { "success", true } });
}

json admin_plugin_uninstall_response(std::string_view raw_request, std::string_view plugin_id, std::string *set_cookie)
{
    json error;
    if (!require_root(raw_request, error, set_cookie)) {
        return error;
    }
    std::string message;
    if (!schedule_plugin_uninstall(plugin_id, message)) {
        return json({ { "success", false }, { "message", message } });
    }
    // Honest wording: schedule_plugin_uninstall only marks intent and
    // withdraws the plugin from the registry. The package itself is not
    // deleted until the next cold start runs its cleanup entry point
    // (plugin-package-format.md, "生命周期入口").
    return json({ { "success", true }, { "message", "已标记待卸载，包将在下一次冷启动清理后删除" } });
}

json plugin_frontend_entries_response()
{
    json entries = json::array();
    for (const InstalledPackage &package : all_installed_packages()) {
        // A package can be installed but broken (missing entry.js would have
        // failed validate_package_layout at load time and marked it failed);
        // this list only ever offers what is actually there to import.
        if (!validate_frontend_entry(package.dir).empty()) {
            continue;
        }
        entries.push_back(
            json({ { "id", package.id }, { "url", "/api/plugins/frontend/" + package.id + "/entry.js" } }));
    }
    return json({ { "success", true }, { "data", std::move(entries) } });
}

std::optional<fs::path> plugin_frontend_asset(std::string_view plugin_id, std::string_view relative_path)
{
    if (!relative_path_is_safe(relative_path)) {
        return std::nullopt;
    }
    const auto package_dir = package_dir_for_id(plugin_id);
    if (!package_dir.has_value()) {
        return std::nullopt;
    }
    const fs::path frontend_root = *package_dir / "frontend";
    const fs::path candidate = frontend_root / fs::path{ relative_path };
    std::error_code error;
    if (!fs::is_regular_file(candidate, error) || !path_is_within(frontend_root, candidate)) {
        return std::nullopt;
    }
    return candidate;
}

} // namespace revlm::plugin
