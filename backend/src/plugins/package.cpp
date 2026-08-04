#include "plugins/package.hpp"

#include "util/json.hpp"
#include "util/strings.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace revlm::plugin
{
namespace
{

namespace fs = std::filesystem;

std::string read_small_file(const fs::path &path)
{
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > 1024U * 1024U) {
        throw std::runtime_error("plugin metadata is missing or too large: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to read plugin metadata: " + path.string());
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input && !input.eof()) {
        throw std::runtime_error("unable to read plugin metadata: " + path.string());
    }
    return bytes;
}

std::string required_string(const json &object, std::string_view key, bool allow_empty = false)
{
    const auto value = object[key].as_string();
    if (!value.has_value()) {
        throw std::runtime_error("plugin.json is missing the " + std::string{ key } + " field");
    }
    const std::string trimmed = trim_ascii(*value);
    if (!allow_empty && trimmed.empty()) {
        throw std::runtime_error("plugin.json field " + std::string{ key } + " must not be empty");
    }
    return trimmed;
}

// The v1 manifest declares the platform module by directory convention rather
// than in plugin.json. backend/<amd|arm>/ must contain exactly one .so; the
// filename is arbitrary.
std::optional<fs::path> scan_platform_modules(const fs::path &root, std::string_view arch)
{
    if (arch != "amd" && arch != "arm") {
        return std::nullopt;
    }
    const fs::path dir = root / "backend" / std::string{ arch };
    std::error_code error;
    if (!fs::is_directory(dir, error)) {
        return std::nullopt;
    }
    std::vector<fs::path> modules;
    for (const fs::directory_entry &entry : fs::directory_iterator(dir, error)) {
        if (error) {
            return std::nullopt;
        }
        if (entry.is_regular_file(error) && entry.path().extension() == ".so") {
            modules.push_back(entry.path().filename());
        }
    }
    if (modules.size() != 1) {
        return std::nullopt;
    }
    return fs::path{ "backend" } / std::string{ arch } / modules.front();
}

// Single-directory layout: REVLM_PLUGIN_DIR/packages/<id>/ is the package,
// there is no version directory underneath. Only identifier-safe directory
// names are considered packages.
std::vector<fs::path> package_roots(const fs::path &base)
{
    std::vector<fs::path> roots;
    std::error_code error;
    const fs::path package_dir = base / "packages";
    if (!fs::is_directory(package_dir, error)) {
        return roots;
    }
    for (const fs::directory_entry &id_entry : fs::directory_iterator(package_dir, error)) {
        if (error) {
            break;
        }
        if (id_entry.is_directory(error) && plugin_identifier_is_safe(id_entry.path().filename().string())) {
            roots.push_back(id_entry.path());
        }
    }
    return roots;
}

bool marker_file_exists(const fs::path &plugin_dir, std::string_view dir, std::string_view id)
{
    std::error_code error;
    return fs::is_regular_file(plugin_dir / dir / std::string{ id }, error);
}

// A package is enabled unless it carries a disabled or pending-uninstall
// marker. Absence of markers means enabled: system packages default to enabled
// without any marker writes, and a fresh install writes the active marker for
// explicitness.
bool plugin_is_enabled(const fs::path &plugin_dir, std::string_view id)
{
    return !marker_file_exists(plugin_dir, "disabled", id) && !marker_file_exists(plugin_dir, "pending", id);
}

void add_candidate(std::unordered_map<std::string, ActivePlugin> &selected, const fs::path &root, bool system,
                   const PluginPlatform &platform)
{
    try {
        PluginPackage package = read_plugin_package(root);
        const auto module_rel = module_path_for_platform(root, platform);
        if (!module_rel.has_value()) {
            return; // no module for this host platform; absent from this process
        }
        std::error_code error;
        const fs::path module = root / *module_rel;
        if (!fs::is_regular_file(module, error)) {
            return;
        }
        // Copy the id before the aggregate is built: `selected[package.id]` is
        // evaluated after the RHS, which moves package.id (now empty) away.
        const std::string id = package.id;
        selected[id] = ActivePlugin{ std::move(package), root, module, system };
    } catch (const std::exception &) {
        // A package with corrupt metadata is trusted code, but its metadata
        // still cannot be allowed to make the bootstrap itself unusable. It is
        // simply absent from this process. install_plugin_archive validates
        // before publishing, so this only happens after external tampering.
    }
}

} // namespace

PluginPlatform current_plugin_platform()
{
    std::string os;
#if defined(__APPLE__)
    os = "darwin";
#elif defined(__linux__)
    os = "linux";
#else
    os = "unknown";
#endif
    std::string arch;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    arch = "amd";
#elif defined(__aarch64__) || defined(_M_ARM64)
    arch = "arm";
#else
    arch = "unknown";
#endif
    return { std::move(os), std::move(arch) };
}

bool plugin_identifier_is_safe(std::string_view value)
{
    if (value.empty() || value.size() > 128) {
        return false;
    }
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char ch) { return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.'; });
}

PluginPackage read_plugin_package(const fs::path &root)
{
    const auto parsed = json::parse(read_small_file(root / "plugin.json"));
    if (!parsed.has_value() || !parsed->is_object()) {
        throw std::runtime_error("plugin.json is invalid");
    }
    const json &object = *parsed;

    // The v1 manifest carries exactly five fields. Explicitly reject the v2
    // fields so an old package is reported clearly instead of half-parsed.
    for (const char *legacy : { "format_version", "core_abi", "requires", "targets", "load_order", "migrations" }) {
        if (object.contains(legacy)) {
            throw std::runtime_error("plugin.json must not contain " + std::string{ legacy } +
                                     " (v1 manifests have only five fields)");
        }
    }

    PluginPackage package;
    package.id = required_string(object, "id");
    package.type = required_string(object, "type");
    package.name = required_string(object, "name");
    package.description = required_string(object, "description", /*allow_empty=*/true);
    package.version = required_string(object, "version");

    if (!plugin_identifier_is_safe(package.id)) {
        throw std::runtime_error("plugin id is invalid");
    }
    if (!plugin_identifier_is_safe(package.version)) {
        throw std::runtime_error("plugin version is invalid");
    }
    if (package.type != "channel") {
        throw std::runtime_error("unsupported plugin type; only \"channel\" is a valid v1 type");
    }

    // frontend/entry.js must be provided; it may be an empty/no-op ESM module.
    std::error_code error;
    if (!fs::is_regular_file(root / "frontend" / "entry.js", error)) {
        throw std::runtime_error("plugin package is missing frontend/entry.js");
    }
    return package;
}

std::optional<fs::path> module_path_for_platform(const fs::path &root, const PluginPlatform &platform)
{
    return scan_platform_modules(root, platform.arch);
}

std::vector<ActivePlugin> active_plugins(const fs::path &plugin_dir, const fs::path &system_plugin_dir)
{
    const PluginPlatform platform = current_plugin_platform();
    std::unordered_map<std::string, ActivePlugin> selected;

    // System packages first; user packages override them by manifest id.
    for (const fs::path &root : package_roots(system_plugin_dir)) {
        add_candidate(selected, root, /*system=*/true, platform);
    }
    for (const fs::path &root : package_roots(plugin_dir)) {
        add_candidate(selected, root, /*system=*/false, platform);
    }
    for (auto it = selected.begin(); it != selected.end();) {
        if (plugin_is_enabled(plugin_dir, it->first)) {
            ++it;
        } else {
            it = selected.erase(it);
        }
    }

    std::vector<ActivePlugin> out;
    out.reserve(selected.size());
    for (auto &entry : selected) {
        out.push_back(std::move(entry.second));
    }
    std::sort(out.begin(), out.end(),
              [](const ActivePlugin &left, const ActivePlugin &right) { return left.package.id < right.package.id; });
    return out;
}

} // namespace revlm::plugin
