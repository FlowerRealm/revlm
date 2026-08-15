#include "plugins/scan.hpp"

#include "config/config.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <unordered_map>

#if defined(__x86_64__) || defined(_M_X64)
#define REVLM_PLUGIN_PLATFORM "amd"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define REVLM_PLUGIN_PLATFORM "arm"
#else
#error "unsupported host architecture: add an amd/arm mapping in plugins/scan.cpp"
#endif

namespace revlm::plugin
{
namespace
{

namespace fs = std::filesystem;

bool is_valid_plugin_id(std::string_view value)
{
    if (value.empty()) {
        return false;
    }
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.'; });
}

} // namespace

std::string current_platform_name()
{
    return REVLM_PLUGIN_PLATFORM;
}

std::string validate_platform_dir(const fs::path &package_dir, std::string_view platform_name)
{
    const fs::path platform_dir = package_dir / "backend" / platform_name;
    std::error_code error;
    if (!fs::is_directory(platform_dir, error)) {
        return "missing backend/" + std::string{ platform_name } + "/ directory";
    }

    int so_count = 0;
    for (const fs::directory_entry &entry : fs::directory_iterator(platform_dir, error)) {
        if (error) {
            return "unable to read backend/" + std::string{ platform_name } + "/";
        }
        if (entry.path().extension() == ".so" && entry.is_regular_file(error)) {
            ++so_count;
        }
    }
    if (error) {
        return "unable to read backend/" + std::string{ platform_name } + "/";
    }
    if (so_count == 0) {
        return "backend/" + std::string{ platform_name } + "/ has no loadable .so module";
    }
    if (so_count > 1) {
        return "backend/" + std::string{ platform_name } + "/ has more than one .so -- exactly one is required";
    }
    return {};
}

std::string validate_frontend_entry(const fs::path &package_dir)
{
    std::error_code error;
    if (!fs::is_regular_file(package_dir / "frontend" / "entry.js", error)) {
        return "missing frontend/entry.js";
    }
    return {};
}

std::string validate_package_layout(const fs::path &package_dir)
{
    if (auto err = validate_platform_dir(package_dir, current_platform_name()); !err.empty()) {
        return err;
    }
    return validate_frontend_entry(package_dir);
}

std::vector<InstalledPackage> installed_packages(const fs::path &base)
{
    std::vector<InstalledPackage> out;
    std::error_code error;
    const fs::path packages_dir = base / "packages";
    if (!fs::is_directory(packages_dir, error)) {
        return out;
    }
    for (const fs::directory_entry &entry : fs::directory_iterator(packages_dir, error)) {
        if (error) {
            break;
        }
        const std::string id = entry.path().filename().string();
        if (!entry.is_directory(error) || !is_valid_plugin_id(id)) {
            // Leftovers from the deleted version-directory / active / disabled
            // design, or simply not a package -- a scan does not interpret
            // them, it only lists valid package directories.
            continue;
        }
        out.push_back(InstalledPackage{ id, entry.path() });
    }
    std::sort(out.begin(), out.end(), [](const InstalledPackage &a, const InstalledPackage &b) { return a.id < b.id; });
    return out;
}

std::vector<InstalledPackage> all_installed_packages()
{
    std::unordered_map<std::string, InstalledPackage> merged;
    // System packages first; a user-installed package of the same id then
    // overwrites it below. config.hpp: root may enable/disable a system
    // package but its files stay part of the image, so it never wins over an
    // upload of the same id.
    for (InstalledPackage &pkg : installed_packages(config().system_plugin_dir)) {
        merged.emplace(pkg.id, std::move(pkg));
    }
    for (InstalledPackage &pkg : installed_packages(config().plugin_dir)) {
        merged.insert_or_assign(pkg.id, std::move(pkg));
    }

    std::vector<InstalledPackage> out;
    out.reserve(merged.size());
    for (auto &[id, pkg] : merged) {
        out.push_back(std::move(pkg));
    }
    std::sort(out.begin(), out.end(), [](const InstalledPackage &a, const InstalledPackage &b) { return a.id < b.id; });
    return out;
}

} // namespace revlm::plugin
