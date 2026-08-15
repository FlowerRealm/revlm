#include "plugins/host.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <odb/transaction.hxx>

#include "config/config.hpp"
#include "plugins/abi.h"
#include "plugins/manifest.hpp"
#include "plugins/registry.hpp"
#include "plugins/scan.hpp"
#include "store/database.hpp"
#include "util/json.hpp"

#include "registry_internal.hpp"

namespace revlm::plugin
{

namespace
{

namespace fs = std::filesystem;

constexpr const char *k_status_loaded = "loaded";
constexpr const char *k_status_disabled = "disabled";
constexpr const char *k_status_failed = "failed";
constexpr const char *k_status_pending_uninstall = "pending_uninstall";

struct KnownPlugin {
    PluginManifest manifest;
    fs::path dir;
    /*
     * Kept for the lifetime of the process. Disabling a plugin withdraws its
     * registry entries but does not dlclose: unloading code that a request might
     * still be inside is the hot-reload hazard ADR 0001 rules out, and keeping
     * the handle is what lets re-enabling be a plain re-registration.
     */
    void *handle = nullptr;
    std::string status;
    std::string error;
};

std::mutex &state_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, KnownPlugin> &known_plugins()
{
    static std::map<std::string, KnownPlugin> plugins;
    return plugins;
}

/*
 * The server ordinary global endpoints land on. Retained from load_plugins()
 * because enabling a plugin later has to register onto the same one, and there
 * is exactly one server per process.
 */
::httplib::Server *&bound_server()
{
    static ::httplib::Server *server = nullptr;
    return server;
}

/*
 * Enable/disable and pending-uninstall intent.
 *
 * Deliberately not the filesystem: an `active/` symlink and a `disabled/` marker
 * were two more places for the truth to live, and ADR 0001 rejects that
 * duplication. Deliberately not a database table either -- v3 deleted
 * `plugin_installations`, and re-adding a table to hold two string lists would
 * undo that for nothing. One small JSON file next to the packages is the whole
 * mechanism.
 */
struct PluginState {
    std::vector<std::string> disabled;
    std::vector<std::string> pending_uninstall;
};

fs::path state_path()
{
    return fs::path{ config().plugin_dir } / "state.json";
}

bool contains(const std::vector<std::string> &values, std::string_view value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<std::string> string_list(const json &value)
{
    std::vector<std::string> out;
    if (!value.is_array()) {
        return out;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (auto text = value[index].as_string()) {
            out.push_back(std::move(*text));
        }
    }
    return out;
}

PluginState load_state()
{
    PluginState state;
    std::ifstream input(state_path(), std::ios::binary);
    if (!input) {
        // No file yet means no intent recorded yet: everything installed is
        // enabled and nothing is pending removal.
        return state;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto parsed = json::parse(buffer.str());
    if (!parsed) {
        return state;
    }
    state.disabled = string_list((*parsed)["disabled"]);
    state.pending_uninstall = string_list((*parsed)["pending_uninstall"]);
    return state;
}

bool save_state(const PluginState &state, std::string &error)
{
    json disabled = json::array();
    for (const auto &id : state.disabled) {
        disabled.push_back(json(id));
    }
    json pending = json::array();
    for (const auto &id : state.pending_uninstall) {
        pending.push_back(json(id));
    }
    json document;
    document["disabled"] = std::move(disabled);
    document["pending_uninstall"] = std::move(pending);

    // Write beside the target and rename, so a crash mid-write cannot leave a
    // truncated file that would read as "nothing disabled".
    const fs::path target = state_path();
    const fs::path temporary = target.string() + ".tmp";
    std::error_code code;
    fs::create_directories(target.parent_path(), code);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "unable to write plugin state file " + temporary.string();
            return false;
        }
        const std::string text = document.dump();
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output) {
            error = "unable to write plugin state file " + temporary.string();
            return false;
        }
    }
    fs::rename(temporary, target, code);
    if (code) {
        error = "unable to publish plugin state file: " + code.message();
        return false;
    }
    return true;
}

std::string read_file(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

fs::path module_path(const fs::path &package_dir)
{
    const fs::path platform_dir = package_dir / "backend" / current_platform_name();
    std::error_code code;
    for (const auto &entry : fs::directory_iterator(platform_dir, code)) {
        if (entry.is_regular_file() && entry.path().extension() == ".so") {
            return entry.path();
        }
    }
    return {};
}

/*
 * One plugin, one dlopen. RTLD_LOCAL keeps its symbols out of the global
 * namespace, so two plugins exporting the same name never collide and a
 * lifecycle call always reaches the module it was aimed at. RTLD_NOW surfaces a
 * missing core symbol here, as a load failure attributable to this plugin,
 * rather than as a crash on the first request that touches it.
 */
void *open_module(const fs::path &module, std::string &error)
{
    ::dlerror();
    void *handle = ::dlopen(module.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char *reason = ::dlerror();
        error = "unable to load " + module.string() + ": " + (reason != nullptr ? reason : "unknown error");
    }
    return handle;
}

template <class Fn> Fn lookup(void *handle, const char *symbol)
{
    ::dlerror();
    void *address = ::dlsym(handle, symbol);
    return address == nullptr ? nullptr : reinterpret_cast<Fn>(address);
}

/*
 * Each plugin's migration runs in its own transaction, so a failure rolls back
 * only that plugin and cannot leave half a schema behind. That per-plugin
 * granularity is the reason lifecycle symbols are resolved explicitly instead of
 * through one shared call (ADR 0006).
 */
bool run_migrate(void *handle, std::string &error)
{
    const auto migrate = lookup<revlm_plugin_migrate_fn>(handle, REVLM_PLUGIN_MIGRATE_SYMBOL);
    if (migrate == nullptr) {
        return true; // A missing symbol is a no-op, not a failure.
    }
    char message[REVLM_PLUGIN_ERROR_SIZE] = { 0 };
    try {
        odb::transaction transaction(database().begin());
        if (migrate(message, sizeof(message)) != 0) {
            error = message[0] != '\0' ? message : "plugin migration failed without a message";
            return false;
        }
        transaction.commit();
    } catch (const std::exception &failure) {
        error = std::string("plugin migration failed: ") + failure.what();
        return false;
    }
    return true;
}

bool run_register(const std::string &id, void *handle, std::string &error)
{
    const auto register_fn = lookup<revlm_plugin_register_fn>(handle, REVLM_PLUGIN_REGISTER_SYMBOL);
    if (register_fn == nullptr) {
        error = "missing " + std::string{ REVLM_PLUGIN_REGISTER_SYMBOL };
        return false;
    }
    if (bound_server() == nullptr) {
        error = "plugin registration attempted before the server was bound";
        return false;
    }
    const revlm_plugin_services services = make_plugin_services(id, *bound_server());
    char message[REVLM_PLUGIN_ERROR_SIZE] = { 0 };
    if (register_fn(&services, message, sizeof(message)) != 0) {
        error = message[0] != '\0' ? message : "plugin registration failed without a message";
        // Anything the plugin managed to register before failing must not stay
        // behind: a half-registered plugin would serve some routes and not
        // others, which is harder to diagnose than not loading at all.
        unregister_plugin(id);
        return false;
    }
    return true;
}

bool run_cleanup(const fs::path &package_dir, std::string &error)
{
    const fs::path module = module_path(package_dir);
    if (module.empty()) {
        // Nothing loadable left to ask. Removal proceeds.
        return true;
    }
    void *handle = open_module(module, error);
    if (handle == nullptr) {
        return false;
    }
    const auto cleanup = lookup<revlm_plugin_cleanup_fn>(handle, REVLM_PLUGIN_CLEANUP_SYMBOL);
    if (cleanup == nullptr) {
        ::dlclose(handle);
        return true;
    }
    char message[REVLM_PLUGIN_ERROR_SIZE] = { 0 };
    const int status = cleanup(message, sizeof(message));
    ::dlclose(handle);
    if (status != 0) {
        error = message[0] != '\0' ? message : "plugin cleanup failed without a message";
        return false;
    }
    return true;
}

void record_failure(KnownPlugin &plugin, std::string error)
{
    plugin.status = k_status_failed;
    plugin.error = std::move(error);
    unregister_plugin(plugin.manifest.id);
}

} // namespace

void load_plugins(::httplib::Server &server)
{
    const std::lock_guard<std::mutex> guard(state_mutex());
    bound_server() = &server;
    PluginState state = load_state();
    auto &plugins = known_plugins();
    plugins.clear();

    // Uninstalls first: a package on its way out must not be migrated or
    // registered on the way past.
    std::vector<std::string> still_pending;
    for (const auto &id : state.pending_uninstall) {
        const auto packages = all_installed_packages();
        const auto found = std::find_if(packages.begin(), packages.end(),
                                        [&](const InstalledPackage &package) { return package.id == id; });
        if (found == packages.end()) {
            continue; // Already gone.
        }
        std::string error;
        if (!run_cleanup(found->dir, error)) {
            // Keep the package and report it. Deleting data a plugin failed to
            // clean up would destroy the only thing a retry could work with.
            KnownPlugin plugin;
            plugin.manifest.id = id;
            plugin.dir = found->dir;
            plugin.status = k_status_failed;
            plugin.error = std::move(error);
            plugins.emplace(id, std::move(plugin));
            still_pending.push_back(id);
            continue;
        }
        std::error_code code;
        fs::remove_all(found->dir, code);
    }
    if (still_pending != state.pending_uninstall) {
        state.pending_uninstall = still_pending;
        std::string ignored;
        save_state(state, ignored);
    }

    for (const auto &package : all_installed_packages()) {
        if (plugins.contains(package.id)) {
            continue; // Failed cleanup above; do not also try to load it.
        }
        KnownPlugin plugin;
        plugin.dir = package.dir;
        plugin.manifest.id = package.id;

        const auto manifest = parse_manifest(read_file(package.dir / "plugin.json"), package.dir.string());
        if (!manifest) {
            record_failure(plugin, manifest.error());
            plugins.emplace(package.id, std::move(plugin));
            continue;
        }
        plugin.manifest = *manifest;

        if (const std::string layout = validate_package_layout(package.dir); !layout.empty()) {
            record_failure(plugin, layout);
            plugins.emplace(package.id, std::move(plugin));
            continue;
        }

        if (contains(state.disabled, package.id)) {
            // Not loaded at all while disabled, so its migration waits for the
            // cold start after it is enabled again.
            plugin.status = k_status_disabled;
            plugins.emplace(package.id, std::move(plugin));
            continue;
        }

        std::string error;
        plugin.handle = open_module(module_path(package.dir), error);
        if (plugin.handle == nullptr) {
            record_failure(plugin, std::move(error));
            plugins.emplace(package.id, std::move(plugin));
            continue;
        }
        if (!run_migrate(plugin.handle, error) || !run_register(package.id, plugin.handle, error)) {
            record_failure(plugin, std::move(error));
            plugins.emplace(package.id, std::move(plugin));
            continue;
        }
        plugin.status = k_status_loaded;
        plugins.emplace(package.id, std::move(plugin));
    }
}

std::vector<PluginInfo> plugin_infos()
{
    const std::lock_guard<std::mutex> guard(state_mutex());
    const PluginState state = load_state();
    std::vector<PluginInfo> out;
    out.reserve(known_plugins().size());
    for (const auto &[id, plugin] : known_plugins()) {
        PluginInfo info;
        info.id = plugin.manifest.id;
        info.type = plugin.manifest.type;
        info.name = plugin.manifest.name;
        info.description = plugin.manifest.description;
        info.version = plugin.manifest.version;
        info.dir = plugin.dir.string();
        info.status = contains(state.pending_uninstall, id) && plugin.status != k_status_failed ?
                          k_status_pending_uninstall :
                          plugin.status;
        info.error = plugin.error;
        out.push_back(std::move(info));
    }
    return out;
}

bool set_plugin_enabled(std::string_view plugin_id, bool enabled, std::string &error)
{
    const std::lock_guard<std::mutex> guard(state_mutex());
    auto &plugins = known_plugins();
    const auto found = plugins.find(std::string{ plugin_id });
    if (found == plugins.end()) {
        error = "unknown plugin " + std::string{ plugin_id };
        return false;
    }
    if (found->second.status == k_status_failed) {
        error = "plugin " + std::string{ plugin_id } + " failed to load: " + found->second.error;
        return false;
    }

    PluginState state = load_state();
    const bool currently_disabled = contains(state.disabled, plugin_id);
    if (enabled == !currently_disabled) {
        return true; // Already in the requested state.
    }

    if (enabled) {
        // The module may not be open yet: a plugin disabled at startup was never
        // loaded, so enabling it has to open and register it now. Its migration
        // still waits for the next cold start.
        if (found->second.handle == nullptr) {
            found->second.handle = open_module(module_path(found->second.dir), error);
            if (found->second.handle == nullptr) {
                record_failure(found->second, error);
                return false;
            }
        }
        if (!run_register(found->second.manifest.id, found->second.handle, error)) {
            record_failure(found->second, error);
            return false;
        }
        found->second.status = k_status_loaded;
        std::erase(state.disabled, std::string{ plugin_id });
    } else {
        // Withdrawing registry entries takes effect at once; requests already in
        // flight keep running because nothing loaded is replaced or unloaded.
        unregister_plugin(plugin_id);
        found->second.status = k_status_disabled;
        state.disabled.emplace_back(plugin_id);
    }
    return save_state(state, error);
}

bool schedule_plugin_uninstall(std::string_view plugin_id, std::string &error)
{
    const std::lock_guard<std::mutex> guard(state_mutex());
    if (!known_plugins().contains(std::string{ plugin_id })) {
        error = "unknown plugin " + std::string{ plugin_id };
        return false;
    }
    PluginState state = load_state();
    if (!contains(state.pending_uninstall, plugin_id)) {
        state.pending_uninstall.emplace_back(plugin_id);
    }
    // Withdraw it from service now; the package and its data survive until the
    // next cold start runs cleanup, which is the only place a plugin gets to
    // remove what it created.
    unregister_plugin(plugin_id);
    return save_state(state, error);
}

} // namespace revlm::plugin
