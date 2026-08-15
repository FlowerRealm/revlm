#include "plugins/registry.hpp"

#include "plugins/data_plane.hpp"
#include "registry_internal.hpp"

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * revlm_plugin_host is opaque to plugins (forward-declared in plugins/abi.h);
 * its body lives here because the registry is the only code that ever
 * dereferences it. It carries just enough for the three control-plane calls
 * below to find their way back into the registry and onto the shared
 * httplib::Server that ordinary endpoints register onto.
 */
struct revlm_plugin_host {
    std::string plugin_id;
    ::httplib::Server *server = nullptr;
};

namespace revlm::plugin
{
namespace
{

// (method, path, ChannelGroup.type) -> the one handler serving that key.
// std::map gives tuple<string,string,string> a working operator< for free, and
// registration is cold-start-rate, so there is nothing to gain from a hand
// rolled hash over three strings.
using RouteKey = std::tuple<std::string, std::string, std::string>;

struct RouteEntry {
    ProtocolHandler handler;
    std::string plugin_id;
};

/*
 * "/v1/models/gpt-5.5" -> {"/v1/models/", "/v1/", "/"}: every registrable prefix
 * of this path, longest first. A plugin that registers one of them serves every
 * path under it.
 */
std::vector<std::string_view> trailing_slash_prefixes(std::string_view path)
{
    std::vector<std::string_view> prefixes;
    for (std::size_t cut = path.size(); cut > 0;) {
        const std::size_t slash = path.rfind('/', cut - 1);
        if (slash == std::string_view::npos) {
            break;
        }
        prefixes.push_back(path.substr(0, slash + 1));
        cut = slash;
    }
    return prefixes;
}

// One plugin's contribution to a ChannelGroup.type's model catalogue. Kept
// per plugin, not merged on write, so unregister_plugin can drop exactly this
// plugin's entries without touching what other plugins registered for the
// same group type.
struct ModelEntry {
    std::string plugin_id;
    json models; // array; entries are the plugin's own shape, never parsed here
};

/*
 * The registry: route table, model catalogue, endpoint ownership and the
 * services tables handed to plugins. A function-local static in instance()
 * sidesteps static-init-order hazards between translation units -- every
 * public function in this file goes through it.
 */
class Registry {
public:
    static Registry &instance()
    {
        static Registry registry;
        return registry;
    }

    bool register_route(std::string_view plugin_id, std::string_view method, std::string_view path,
                        std::string_view group_type, ProtocolHandler handler)
    {
        RouteKey key{ std::string(method), std::string(path), std::string(group_type) };
        std::unique_lock lock(mutex_);
        // try_emplace only inserts when the key is free. A key already held by
        // *another* plugin fails here instead of being overwritten: the late
        // registrant becomes a failed plugin, never a silent loser to load order.
        //
        // The same plugin re-registering its own key is not that conflict, it is
        // a reload, and it refreshes the entry. Failing it would make a second
        // load_plugins() call unregister a plugin that is working perfectly --
        // the load would report failure and take its own routes down with it.
        const auto [it, inserted] = routes_.try_emplace(std::move(key), RouteEntry{ handler, std::string(plugin_id) });
        if (!inserted) {
            if (it->second.plugin_id != plugin_id) {
                return false;
            }
            it->second.handler = handler;
        }
        active_.insert(std::string(plugin_id));
        return true;
    }

    bool register_models(std::string_view plugin_id, std::string_view group_type, std::string_view models_json)
    {
        // Confirm it is an array and nothing more -- pricing and every other
        // field are the plugin's own shape, so the core never looks inside.
        const auto parsed = json::parse(models_json);
        if (!parsed.has_value() || !parsed->is_array()) {
            return false;
        }

        std::unique_lock lock(mutex_);
        auto &entries = models_[std::string(group_type)];
        const auto existing = std::find_if(entries.begin(), entries.end(),
                                           [&](const ModelEntry &entry) { return entry.plugin_id == plugin_id; });
        if (existing != entries.end()) {
            existing->models = *parsed;
        } else {
            entries.push_back(ModelEntry{ std::string(plugin_id), *parsed });
        }
        active_.insert(std::string(plugin_id));
        return true;
    }

    bool register_endpoint(std::string_view plugin_id, std::string_view method, std::string_view path,
                           EndpointHandler handler, ::httplib::Server &server)
    {
        std::string verb(method);
        std::transform(verb.begin(), verb.end(), verb.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        if (verb != "GET" && verb != "POST" && verb != "PUT" && verb != "DELETE" && verb != "PATCH" &&
            verb != "OPTIONS") {
            return false;
        }

        {
            std::unique_lock lock(mutex_);
            active_.insert(std::string(plugin_id));
        }

        // httplib cannot withdraw a route once registered, so the wrapper checks
        // liveness on every call; disabling the owner then reads as a 404
        // instead of an actual removal, matching what unregister_plugin does
        // for data-plane routes and the model catalogue.
        ::httplib::Server::Handler wrapper = [owner = std::string(plugin_id), handler](const ::httplib::Request &req,
                                                                                       ::httplib::Response &res) {
            if (!Registry::instance().plugin_is_active(owner)) {
                res.status = 404;
                return;
            }
            handler(req, res);
        };

        const std::string route_path(path);
        if (verb == "GET") {
            server.Get(route_path, wrapper);
        } else if (verb == "POST") {
            server.Post(route_path, wrapper);
        } else if (verb == "PUT") {
            server.Put(route_path, wrapper);
        } else if (verb == "DELETE") {
            server.Delete(route_path, wrapper);
        } else if (verb == "PATCH") {
            server.Patch(route_path, wrapper);
        } else {
            server.Options(route_path, wrapper);
        }
        return true;
    }

    ProtocolHandler find_protocol_handler(std::string_view method, std::string_view path, std::string_view group_type)
    {
        std::shared_lock lock(mutex_);
        const auto exact = routes_.find(RouteKey{ std::string(method), std::string(path), std::string(group_type) });
        if (exact != routes_.end()) {
            return exact->second.handler;
        }
        // A registered path ending in '/' claims everything below it, longest
        // first. That is the whole of the core's URL vocabulary: it still parses
        // nothing out of the path, so a sub-resource id ("/v1/models/gpt-5.5")
        // stays the plugin's to read off ctx.request.path. Without it a protocol
        // with per-item endpoints would need one registration per item.
        for (std::string_view prefix : trailing_slash_prefixes(path)) {
            const auto it = routes_.find(RouteKey{ std::string(method), std::string(prefix), std::string(group_type) });
            if (it != routes_.end()) {
                return it->second.handler;
            }
        }
        return nullptr;
    }

    /*
     * Whether any plugin serves this (method, path), whatever the group type.
     * The catch-all HTTP route asks this before authenticating: without it an
     * unknown /api path would answer 401 instead of 404, because the core's own
     * routes are registered first and everything else falls through to here.
     */
    bool has_route(std::string_view method, std::string_view path)
    {
        std::shared_lock lock(mutex_);
        if (has_exact_route(method, path)) {
            return true;
        }
        for (std::string_view prefix : trailing_slash_prefixes(path)) {
            if (has_exact_route(method, prefix)) {
                return true;
            }
        }
        return false;
    }

    /* Caller holds the lock. Any group type will do: the catch-all only asks
     * whether the path exists at all, before it knows the caller's token. */
    bool has_exact_route(std::string_view method, std::string_view path)
    {
        const auto lower = routes_.lower_bound(RouteKey{ std::string(method), std::string(path), std::string{} });
        return lower != routes_.end() && std::get<0>(lower->first) == method && std::get<1>(lower->first) == path;
    }

    json models_for_group_type(std::string_view group_type)
    {
        std::shared_lock lock(mutex_);
        json out = json::array();
        const auto it = models_.find(std::string(group_type));
        if (it == models_.end()) {
            return out;
        }
        for (const auto &entry : it->second) {
            append_all(out, entry.models);
        }
        return out;
    }

    json all_models()
    {
        std::shared_lock lock(mutex_);
        json out = json::array();
        for (const auto &[group_type, entries] : models_) {
            for (const auto &entry : entries) {
                append_all(out, entry.models);
            }
        }
        return out;
    }

    void unregister_plugin(std::string_view plugin_id)
    {
        std::unique_lock lock(mutex_);
        std::erase_if(routes_, [&](const auto &item) { return item.second.plugin_id == plugin_id; });
        for (auto &[group_type, entries] : models_) {
            std::erase_if(entries, [&](const ModelEntry &entry) { return entry.plugin_id == plugin_id; });
        }
        active_.erase(std::string(plugin_id));
    }

    bool plugin_is_active(std::string_view plugin_id)
    {
        std::shared_lock lock(mutex_);
        return active_.contains(std::string(plugin_id));
    }

    // Returns the (registry-owned, process-lifetime) host handle for this
    // plugin id, creating it on first use.
    revlm_plugin_host *host_for(std::string_view plugin_id, ::httplib::Server &server)
    {
        std::unique_lock lock(mutex_);
        auto &slot = hosts_[std::string(plugin_id)];
        if (!slot) {
            slot = std::make_unique<revlm_plugin_host>();
            slot->plugin_id = std::string(plugin_id);
        }
        slot->server = &server;
        return slot.get();
    }

private:
    static void append_all(json &out, const json &models)
    {
        for (std::size_t i = 0; i < models.size(); ++i) {
            out.push_back(models[i]);
        }
    }

    std::shared_mutex mutex_;
    std::map<RouteKey, RouteEntry> routes_;
    std::unordered_map<std::string, std::vector<ModelEntry>> models_;
    std::unordered_set<std::string> active_;
    std::unordered_map<std::string, std::unique_ptr<revlm_plugin_host>> hosts_;
};

// Function-pointer table handed across the C ABI (plugins/abi.h). Every one
// of these catches everything at the boundary: an exception unwinding into a
// plugin's dlopen'd code is undefined behaviour (ADR 0008), so a failure here
// becomes a non-zero return instead.
extern "C" {

int route_thunk(revlm_plugin_host *host, const char *method, const char *path, const char *group_type,
                revlm_plugin_fn handler) noexcept
{
    if (host == nullptr || method == nullptr || path == nullptr || group_type == nullptr || handler == nullptr) {
        return 1;
    }
    try {
        return Registry::instance().register_route(host->plugin_id, method, path, group_type,
                                                   protocol_handler_from(handler)) ?
                   0 :
                   1;
    } catch (...) {
        return 1;
    }
}

int models_thunk(revlm_plugin_host *host, const char *group_type, const char *models_json) noexcept
{
    if (host == nullptr || group_type == nullptr || models_json == nullptr) {
        return 1;
    }
    try {
        return Registry::instance().register_models(host->plugin_id, group_type, models_json) ? 0 : 1;
    } catch (...) {
        return 1;
    }
}

int endpoint_thunk(revlm_plugin_host *host, const char *method, const char *path, revlm_plugin_fn handler) noexcept
{
    if (host == nullptr || method == nullptr || path == nullptr || handler == nullptr || host->server == nullptr) {
        return 1;
    }
    try {
        return Registry::instance().register_endpoint(host->plugin_id, method, path, endpoint_handler_from(handler),
                                                      *host->server) ?
                   0 :
                   1;
    } catch (...) {
        return 1;
    }
}

} // extern "C"

} // namespace

bool has_route(std::string_view method, std::string_view path)
{
    return Registry::instance().has_route(method, path);
}

ProtocolHandler find_protocol_handler(std::string_view method, std::string_view path, std::string_view group_type)
{
    return Registry::instance().find_protocol_handler(method, path, group_type);
}

json models_for_group_type(std::string_view group_type)
{
    return Registry::instance().models_for_group_type(group_type);
}

json all_models()
{
    return Registry::instance().all_models();
}

void unregister_plugin(std::string_view plugin_id)
{
    Registry::instance().unregister_plugin(plugin_id);
}

bool plugin_is_active(std::string_view plugin_id)
{
    return Registry::instance().plugin_is_active(plugin_id);
}

revlm_plugin_services make_plugin_services(std::string_view plugin_id, ::httplib::Server &server)
{
    revlm_plugin_services services{};
    services.abi_version = REVLM_PLUGIN_ABI_VERSION;
    services.host = Registry::instance().host_for(plugin_id, server);
    services.register_route = route_thunk;
    services.register_models = models_thunk;
    services.register_endpoint = endpoint_thunk;
    return services;
}

} // namespace revlm::plugin
