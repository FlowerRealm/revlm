#include "plugins/runtime.hpp"

#include "config/config.hpp"
#include "plugins/packages.hpp"
#include "proxy/gateway.hpp"
#include "server/http_dispatch.hpp"
#include "server/http_server.hpp"
#include "store/database.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace revlm::plugin
{
namespace
{

struct LoadedPlugin {
    void *handle = nullptr;
    v1::Plugin *instance = nullptr;
    v1::DestroyPlugin destroy = nullptr;
    std::vector<v1::RegisteredRoute> routes;
    std::vector<v1::ChannelTypeDescriptor> channel_types;

    ~LoadedPlugin()
    {
        if (instance != nullptr && destroy != nullptr) {
            destroy(instance);
        }
        if (handle != nullptr) {
            ::dlclose(handle);
        }
    }
};

struct RuntimeState {
    std::vector<std::unique_ptr<LoadedPlugin>> plugins;
    std::vector<v1::RegisteredRoute> routes;
    std::vector<v1::ChannelTypeDescriptor> channel_types;
    std::vector<json> channel_schemas;
    std::mutex mutex;
    bool loaded = false;
};

RuntimeState &state()
{
    static RuntimeState value;
    return value;
}

std::string route_key(const v1::RouteDescriptor &route)
{
    return route.method + " " + route.path;
}

void validate_route(const v1::RouteDescriptor &route)
{
    if (route.method != "GET" && route.method != "POST") {
        throw std::runtime_error("plugin route method is not supported");
    }
    if (route.path.empty() || route.path.front() != '/' || route.path.rfind("/v1/", 0) != 0) {
        throw std::runtime_error("plugin route must be under /v1/");
    }
}

void validate_channel_type(const v1::ChannelTypeDescriptor &descriptor)
{
    if (descriptor.type_id.empty() || !descriptor.frontend_schema.is_object() || !descriptor.prepare_upstream) {
        throw std::runtime_error("plugin channel type is incomplete");
    }
}

json read_frontend_schema(const ActivePlugin &package)
{
    std::ifstream input(package.root / package.package.frontend_schema, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to read plugin frontend schema");
    }
    std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto parsed = json::parse(source);
    if (!parsed.has_value() || !parsed->is_object() || !(*parsed)["channel_types"].is_array()) {
        throw std::runtime_error("plugin frontend schema is invalid");
    }
    return *parsed;
}

void validate_schema_for_types(const json &schema, const std::vector<v1::ChannelTypeDescriptor> &types)
{
    for (const auto &descriptor : types) {
        bool found = false;
        for (std::size_t index = 0; index < schema["channel_types"].size(); ++index) {
            const json item = schema["channel_types"][index];
            if (item.is_object() && item["type"].as_string().value_or("") == descriptor.type_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error("frontend schema has no channel type: " + descriptor.type_id);
        }
    }
}

json schema_for_type(const json &schema, std::string_view type_id)
{
    for (std::size_t index = 0; index < schema["channel_types"].size(); ++index) {
        const json item = schema["channel_types"][index];
        if (item.is_object() && item["type"].as_string().value_or("") == type_id) {
            return item["schema"];
        }
    }
    throw std::runtime_error("frontend schema has no channel type: " + std::string{ type_id });
}

std::string unsupported_parameter_name(std::string_view body)
{
    static const std::regex pattern("unsupported parameter[^a-z0-9_]+([a-z0-9_]+)", std::regex_constants::icase);
    std::smatch match;
    const std::string haystack{ body };
    return std::regex_search(haystack, match, pattern) && match.size() >= 2 ? lowercase_ascii(match[1].str()) : "";
}

bool rewrite_body_field(std::string_view body, std::string_view source_name, std::string_view destination_name,
                        bool keep_destination, std::string &out)
{
    const auto parsed = json::parse(body);
    if (!parsed.has_value() || !parsed->is_object() || !parsed->contains(source_name)) {
        return false;
    }
    json document = *parsed;
    json value = document[source_name];
    document.erase(source_name);
    if (!keep_destination || !document.contains(destination_name)) {
        document[destination_name] = std::move(value);
    }
    out = document.dump();
    return out != body;
}

bool remove_body_field(std::string_view body, std::string_view name, std::string &out)
{
    const auto parsed = json::parse(body);
    if (!parsed.has_value() || !parsed->is_object() || !parsed->contains(name)) {
        return false;
    }
    json document = *parsed;
    document.erase(name);
    out = document.dump();
    return true;
}

void commit_usage(ProxyRequest &proxy)
{
    proxy_stream_commit_usage(proxy);
}

} // namespace

void v1::ResponseWriter::write_json(int status, json body)
{
    revlm::write_json(response_, status, std::move(body));
}

void v1::ResponseWriter::write_proxy_result(const json &result)
{
    revlm::write_proxy_result(response_, result);
}

json v1::HostServices::list_models(long long channel_group_id) const
{
    return data_plane_models_response(channel_group_id);
}

json v1::HostServices::retrieve_model(std::string_view model_id, long long channel_group_id, bool &not_found) const
{
    return data_plane_model_retrieve_response(model_id, channel_group_id, not_found);
}

std::function<void(ProxyRequest &)> v1::HostServices::stream_usage_callback() const
{
    return commit_usage;
}

void v1::PluginRegistrar::register_data_plane_route(RouteDescriptor descriptor, DataPlaneHandler &handler)
{
    validate_route(descriptor);
    routes_.push_back({ std::move(descriptor), &handler });
}

void v1::PluginRegistrar::register_channel_type(ChannelTypeDescriptor descriptor)
{
    validate_channel_type(descriptor);
    channel_types_.push_back(std::move(descriptor));
}

void v1::PluginRegistrar::register_migrations(std::vector<std::string> migrations)
{
    migrations_ = std::move(migrations);
}

void load_plugins_for_worker(const std::vector<ActivePlugin> &packages)
{
    RuntimeState &runtime = state();
    std::lock_guard lock(runtime.mutex);
    if (runtime.loaded) {
        return;
    }

    std::unordered_set<std::string> route_keys;
    std::unordered_set<std::string> channel_ids;
    for (const ActivePlugin &package : packages) {
        try {
            auto item = std::make_unique<LoadedPlugin>();
            item->handle = ::dlopen(package.module.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (item->handle == nullptr) {
                throw std::runtime_error(::dlerror());
            }
            auto create = reinterpret_cast<v1::CreatePlugin>(::dlsym(item->handle, "revlm_plugin_create_v1"));
            item->destroy = reinterpret_cast<v1::DestroyPlugin>(::dlsym(item->handle, "revlm_plugin_destroy_v1"));
            if (create == nullptr || item->destroy == nullptr) {
                throw std::runtime_error("plugin factory symbols are missing");
            }
            item->instance = create();
            if (item->instance == nullptr) {
                throw std::runtime_error("plugin factory returned null");
            }

            v1::PluginRegistrar registrar;
            item->instance->register_with(registrar);
            if (registrar.migrations() != package.package.migrations) {
                throw std::runtime_error("plugin migration declaration does not match manifest");
            }
            std::unordered_set<std::string> local_routes;
            std::unordered_set<std::string> local_channels;
            for (const auto &route : registrar.routes()) {
                const std::string key = route_key(route.descriptor);
                if (!local_routes.emplace(key).second || route_keys.contains(key)) {
                    throw std::runtime_error("duplicate plugin route: " + key);
                }
            }
            for (const auto &descriptor : registrar.channel_types()) {
                if (!local_channels.emplace(descriptor.type_id).second || channel_ids.contains(descriptor.type_id)) {
                    throw std::runtime_error("duplicate plugin channel type: " + descriptor.type_id);
                }
            }
            const json schema = read_frontend_schema(package);
            validate_schema_for_types(schema, registrar.channel_types());

            // The module has passed all registry checks. Only now may it change
            // the database, so a rejected module cannot leave partial state.
            apply_plugin_migrations(package);
            set_plugin_runtime_state(package.package.id, "active", "");

            for (const auto &route : registrar.routes()) {
                route_keys.emplace(route_key(route.descriptor));
                item->routes.push_back(route);
                runtime.routes.push_back(route);
            }
            for (const auto &descriptor : registrar.channel_types()) {
                channel_ids.emplace(descriptor.type_id);
                item->channel_types.push_back(descriptor);
                runtime.channel_types.push_back(descriptor);
                runtime.channel_schemas.push_back(schema_for_type(schema, descriptor.type_id));
            }
            runtime.plugins.push_back(std::move(item));
        } catch (const std::exception &error) {
            try {
                set_plugin_runtime_state(package.package.id, "failed", trim_ascii(error.what()));
            } catch (...) {
            }
            std::cerr << "plugin " << package.package.id << " failed to load: " << error.what() << '\n';
        }
    }
    runtime.loaded = true;
}

std::vector<Model> models_for_channel_type(std::string_view channel_type)
{
    RuntimeState &runtime = state();
    std::lock_guard lock(runtime.mutex);
    for (const auto &descriptor : runtime.channel_types) {
        if (descriptor.type_id == channel_type) {
            return descriptor.models;
        }
    }
    return {};
}

std::vector<Model> all_plugin_models()
{
    RuntimeState &runtime = state();
    std::lock_guard lock(runtime.mutex);
    std::vector<Model> out;
    for (const auto &descriptor : runtime.channel_types) {
        out.insert(out.end(), descriptor.models.begin(), descriptor.models.end());
    }
    return out;
}

void prepare_upstream_for_channel(const Channel &channel, const UpstreamRequest &downstream,
                                  UpstreamPreparedRequest &prepared)
{
    RuntimeState &runtime = state();
    std::lock_guard lock(runtime.mutex);
    for (const auto &descriptor : runtime.channel_types) {
        if (descriptor.type_id == channel.type) {
            descriptor.prepare_upstream(channel, downstream, prepared);
            return;
        }
    }
    throw std::runtime_error("no loaded plugin for channel type: " + channel.type);
}

bool retry_upstream_for_channel(const Channel &channel, const UpstreamPreparedRequest &prepared,
                                const UpstreamResponse &response, UpstreamPreparedRequest &retry)
{
    if (channel.type != "openai_compatible" || prepared.retried_unsupported_parameter || response.status_code < 400 ||
        response.status_code >= 500) {
        return false;
    }
    RuntimeState &runtime = state();
    std::lock_guard lock(runtime.mutex);
    const auto descriptor =
        std::find_if(runtime.channel_types.begin(), runtime.channel_types.end(),
                     [&](const v1::ChannelTypeDescriptor &item) { return item.type_id == channel.type; });
    if (descriptor == runtime.channel_types.end() || !descriptor->retry_unsupported_parameter) {
        return false;
    }
    const std::string parameter = unsupported_parameter_name(response.body);
    std::string body;
    bool rewritten = false;
    if (parameter == "max_output_tokens") {
        rewritten = rewrite_body_field(prepared.body, "max_output_tokens", "max_tokens", true, body);
    } else if (parameter == "max_tokens") {
        rewritten = rewrite_body_field(prepared.body, "max_tokens", "max_output_tokens", false, body);
    } else if (parameter == "max_completion_tokens") {
        rewritten = rewrite_body_field(prepared.body, "max_completion_tokens", "max_tokens", true, body);
    } else if (parameter == "stream_options") {
        rewritten = remove_body_field(prepared.body, "stream_options", body);
    }
    if (!rewritten) {
        return false;
    }
    retry = prepared;
    retry.body = std::move(body);
    retry.retried_unsupported_parameter = true;
    return true;
}

json plugin_channel_types_json()
{
    RuntimeState &runtime = state();
    std::lock_guard lock(runtime.mutex);
    json out = json::array();
    for (std::size_t index = 0; index < runtime.channel_types.size(); ++index) {
        const auto &descriptor = runtime.channel_types[index];
        const json schema = index < runtime.channel_schemas.size() ? runtime.channel_schemas[index] : json{};
        out.push_back(json({ { "type", descriptor.type_id },
                             { "name", descriptor.display_name },
                             { "icon", descriptor.icon },
                             { "default_name", descriptor.default_name },
                             { "default_base_url", descriptor.default_base_url },
                             { "schema", schema } }));
    }
    return out;
}

namespace
{

::httplib::Server::Handler plugin_handler(v1::RegisteredRoute route)
{
    return v1_http([route](const ::httplib::Request &, ::httplib::Response &response, ProxyRequest &proxy) {
        v1::AuthenticatedRequest request(proxy);
        v1::ResponseWriter writer(response);
        v1::HostServices host;
        const v1::DataPlaneResult result = route.handler->handle(request, writer, host);
        if (!result.handled_stream && result.commit_usage && proxy.upstream.channel_id > 0) {
            finish_proxy_usage(response, proxy);
        }
    });
}

} // namespace

void register_data_plane_routes(::httplib::Server &server)
{
    RuntimeState &runtime = state();
    std::lock_guard lock(runtime.mutex);
    for (const auto &route : runtime.routes) {
        const auto handler = plugin_handler(route);
        if (route.descriptor.method == "GET") {
            server.Get(route.descriptor.path, handler);
        } else {
            server.Post(route.descriptor.path, handler);
        }
    }
}

} // namespace revlm::plugin
