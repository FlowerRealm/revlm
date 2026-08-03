#include "plugins/package.hpp"

#include "util/json.hpp"
#include "util/strings.hpp"

#include <sys/utsname.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace revlm::plugin
{
namespace
{

namespace fs = std::filesystem;

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool safe_relative_path(std::string_view raw)
{
    if (raw.empty() || raw.front() == '/' || raw.find('\\') != std::string_view::npos ||
        raw.find('\0') != std::string_view::npos) {
        return false;
    }
    const fs::path path{ raw };
    if (path.is_absolute()) {
        return false;
    }
    for (const fs::path &part : path) {
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
    }
    return true;
}

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

std::string required_string(const json &object, std::string_view key)
{
    const auto value = object[key].as_string();
    if (!value.has_value() || trim_ascii(*value).empty()) {
        throw std::runtime_error("plugin.json requires " + std::string{ key });
    }
    return trim_ascii(*value);
}

std::vector<fs::path> package_roots(const fs::path &base)
{
    std::vector<fs::path> roots;
    std::error_code error;
    if (!fs::is_directory(base / "packages", error)) {
        return roots;
    }
    for (const fs::directory_entry &id_entry : fs::directory_iterator(base / "packages", error)) {
        if (error || !id_entry.is_directory(error) || !plugin_identifier_is_safe(id_entry.path().filename().string())) {
            continue;
        }
        std::vector<fs::path> versions;
        for (const fs::directory_entry &version_entry : fs::directory_iterator(id_entry.path(), error)) {
            if (!error && version_entry.is_directory(error) &&
                plugin_identifier_is_safe(version_entry.path().filename().string())) {
                versions.push_back(version_entry.path());
            }
        }
        std::sort(versions.begin(), versions.end());
        if (!versions.empty()) {
            roots.push_back(versions.back());
        }
    }
    return roots;
}

void add_candidate(std::unordered_map<std::string, ActivePlugin> &selected, const fs::path &root, bool system,
                   const PluginPlatform &platform)
{
    try {
        PluginPackage package = read_plugin_package(root);
        const PluginModule *module = module_for_platform(package, platform);
        if (module == nullptr) {
            throw std::runtime_error("no module for " + platform.os + "/" + platform.arch);
        }
        const fs::path module_path = root / module->path;
        std::error_code error;
        if (!fs::is_regular_file(module_path, error)) {
            throw std::runtime_error("declared module is missing: " + module_path.string());
        }
        const std::string id = package.id;
        selected[id] = ActivePlugin{ std::move(package), root, module_path, system };
    } catch (const std::exception &) {
        // A package is trusted code but its metadata still cannot be allowed to
        // make the bootstrap itself unusable. It is simply absent from this
        // process; the control plane reports the detailed error on restart.
    }
}

} // namespace

PluginPlatform current_plugin_platform()
{
    utsname info{};
    if (::uname(&info) != 0) {
        throw std::runtime_error("unable to determine plugin platform");
    }
    std::string os = lowercase(info.sysname);
    std::string arch = lowercase(info.machine);
    if (arch == "x86_64" || arch == "x64") {
        arch = "amd64";
    } else if (arch == "aarch64") {
        arch = "arm64";
    }
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
    if (object["format_version"].as_int64().value_or(0) != 1) {
        throw std::runtime_error("unsupported plugin format_version");
    }

    PluginPackage package;
    package.id = required_string(object, "id");
    package.name = required_string(object, "name");
    package.version = required_string(object, "version");
    package.sdk_abi = required_string(object, "sdk_abi");
    package.frontend_schema = required_string(object, "frontend_schema");
    if (!plugin_identifier_is_safe(package.id) || !plugin_identifier_is_safe(package.version) ||
        package.sdk_abi != k_sdk_abi || !safe_relative_path(package.frontend_schema)) {
        throw std::runtime_error("plugin id, version, SDK ABI, or frontend schema is invalid");
    }
    std::error_code schema_error;
    if (!fs::is_regular_file(root / package.frontend_schema, schema_error) || schema_error) {
        throw std::runtime_error("declared frontend schema is missing");
    }
    const auto schema = json::parse(read_small_file(root / package.frontend_schema));
    if (!schema.has_value() || !schema->is_object() || !schema->contains("channel_types") ||
        !(*schema)["channel_types"].is_array()) {
        throw std::runtime_error("declared frontend schema is invalid");
    }

    const json dependencies = object["requires"];
    if (!dependencies.is_array()) {
        throw std::runtime_error("plugin.json requires must be an array");
    }
    std::unordered_set<std::string> require_ids;
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        const auto id = dependencies[index].as_string();
        if (!id.has_value() || !plugin_identifier_is_safe(*id) || !require_ids.emplace(*id).second ||
            *id == package.id) {
            throw std::runtime_error("plugin.json has an invalid dependency");
        }
        package.dependencies.push_back(*id);
    }

    const json targets = object["targets"];
    if (!targets.is_array() || targets.size() == 0) {
        throw std::runtime_error("plugin.json requires at least one target");
    }
    std::unordered_set<std::string> target_ids;
    for (std::size_t index = 0; index < targets.size(); ++index) {
        const json target = targets[index];
        if (!target.is_object()) {
            throw std::runtime_error("plugin target is invalid");
        }
        PluginModule module{ required_string(target, "os"), required_string(target, "arch"),
                             required_string(target, "module") };
        const std::string target_id = lowercase(module.os) + "/" + lowercase(module.arch);
        if (!plugin_identifier_is_safe(module.os) || !plugin_identifier_is_safe(module.arch) ||
            !safe_relative_path(module.path) || !target_ids.emplace(target_id).second) {
            throw std::runtime_error("plugin target is invalid");
        }
        package.modules.push_back(std::move(module));
    }

    const json migrations = object["migrations"];
    if (!migrations.is_array()) {
        throw std::runtime_error("plugin.json migrations must be an array");
    }
    std::unordered_set<std::string> migration_paths;
    for (std::size_t index = 0; index < migrations.size(); ++index) {
        const auto path = migrations[index].as_string();
        if (!path.has_value() || !safe_relative_path(*path) || !migration_paths.emplace(*path).second) {
            throw std::runtime_error("plugin migration path is invalid");
        }
        package.migrations.push_back(*path);
    }

    if (object.contains("load_order")) {
        throw std::runtime_error("plugin load_order is not supported by format v1");
    }
    return package;
}

const PluginModule *module_for_platform(const PluginPackage &package, const PluginPlatform &platform)
{
    const auto it = std::find_if(package.modules.begin(), package.modules.end(), [&](const PluginModule &module) {
        return lowercase(module.os) == platform.os && lowercase(module.arch) == platform.arch;
    });
    return it == package.modules.end() ? nullptr : &*it;
}

std::vector<ActivePlugin> active_plugins(const fs::path &plugin_dir, const fs::path &system_plugin_dir)
{
    const PluginPlatform platform = current_plugin_platform();
    std::unordered_map<std::string, ActivePlugin> selected;

    for (const fs::path &root : package_roots(system_plugin_dir)) {
        add_candidate(selected, root, true, platform);
    }

    std::error_code error;
    const fs::path active_dir = plugin_dir / "active";
    if (fs::is_directory(active_dir, error)) {
        for (const fs::directory_entry &entry : fs::directory_iterator(active_dir, error)) {
            const std::string id = entry.path().filename().string();
            if (error || !plugin_identifier_is_safe(id) || !entry.is_symlink(error)) {
                continue;
            }
            const fs::path root = fs::weakly_canonical(entry.path(), error);
            if (error || root.empty()) {
                continue;
            }
            add_candidate(selected, root, false, platform);
        }
    }

    const fs::path disabled_dir = plugin_dir / "disabled";
    if (fs::is_directory(disabled_dir, error)) {
        for (const fs::directory_entry &entry : fs::directory_iterator(disabled_dir, error)) {
            const std::string id = entry.path().filename().string();
            if (!error && plugin_identifier_is_safe(id)) {
                selected.erase(id);
            }
        }
    }

    const auto before = [&](const std::string &left, const std::string &right) {
        return selected.at(left).package.id < selected.at(right).package.id;
    };

    enum class Visit { none, visiting, complete, rejected };
    std::unordered_map<std::string, Visit> visits;
    std::function<bool(const std::string &)> visit = [&](const std::string &id) {
        const Visit state = visits[id];
        if (state == Visit::complete) {
            return true;
        }
        if (state == Visit::visiting || state == Visit::rejected) {
            visits[id] = Visit::rejected;
            return false;
        }
        const auto current = selected.find(id);
        if (current == selected.end()) {
            visits[id] = Visit::rejected;
            return false;
        }
        visits[id] = Visit::visiting;
        for (const std::string &dependency : current->second.package.dependencies) {
            if (!visit(dependency)) {
                visits[id] = Visit::rejected;
                return false;
            }
        }
        visits[id] = Visit::complete;
        return true;
    };
    for (const auto &[id, _] : selected) {
        (void)visit(id);
    }

    // An edge points from a package to the package it needs. Keep the order
    // deterministic so the worker snapshot and diagnostics are reproducible.
    std::unordered_map<std::string, int> incoming;
    for (const auto &[id, _] : selected) {
        if (visits[id] == Visit::complete) {
            incoming.emplace(id, 0);
        }
    }
    for (const auto &[id, active] : selected) {
        if (visits[id] != Visit::complete) {
            continue;
        }
        for (const std::string &dependency : active.package.dependencies) {
            if (visits[dependency] != Visit::complete) {
                visits[id] = Visit::rejected;
                continue;
            }
            ++incoming[dependency];
        }
    }

    std::vector<std::string> ready;
    for (const auto &[id, degree] : incoming) {
        if (degree == 0 && visits[id] == Visit::complete) {
            ready.push_back(id);
        }
    }
    std::vector<ActivePlugin> ordered;
    while (!ready.empty()) {
        std::sort(ready.begin(), ready.end(), before);
        const std::string id = ready.front();
        ready.erase(ready.begin());
        ordered.push_back(selected.at(id));
        for (const std::string &dependency : selected.at(id).package.dependencies) {
            if (visits[dependency] != Visit::complete) {
                continue;
            }
            if (--incoming[dependency] == 0) {
                ready.push_back(dependency);
            }
        }
    }
    return ordered;
}

} // namespace revlm::plugin
