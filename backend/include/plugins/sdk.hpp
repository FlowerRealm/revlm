#pragma once

#include <functional>
#include <httplib.h>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "channels/channels.hpp"
#include "models/models.hpp"
#include "proxy/upstream.hpp"
#include "request/proxy_request.hpp"
#include "util/json.hpp"

namespace revlm::plugin::v1
{

inline constexpr std::string_view k_plugin_sdk_abi = "revlm-plugin-cpp-v1";

struct RouteDescriptor {
    std::string method;
    std::string path;
    bool uses_gateway = false;
    bool supports_stream = false;
};

struct DataPlaneResult {
    bool handled_stream = false;
    bool commit_usage = true;
};

struct AuthenticatedRequest {
    explicit AuthenticatedRequest(ProxyRequest &proxy)
        : proxy(proxy)
        , http(proxy.http)
    {
    }

    ProxyRequest &proxy;
    HttpRequest &http;
};

class ResponseWriter {
public:
    explicit ResponseWriter(::httplib::Response &response)
        : response_(response)
    {
    }

    ::httplib::Response &native()
    {
        return response_;
    }

    void write_json(int status, json body);
    void write_proxy_result(const json &result);

private:
    ::httplib::Response &response_;
};

class HostServices {
public:
    json list_models(long long channel_group_id) const;
    json retrieve_model(std::string_view model_id, long long channel_group_id, bool &not_found) const;
    std::function<void(ProxyRequest &)> stream_usage_callback() const;
};

class DataPlaneHandler {
public:
    virtual ~DataPlaneHandler() = default;
    virtual DataPlaneResult handle(AuthenticatedRequest &request, ResponseWriter &response, HostServices &host) = 0;
};

struct ChannelTypeDescriptor {
    std::string type_id;
    std::string display_name;
    std::string icon;
    std::string default_name;
    std::string default_base_url;
    std::vector<Model> models;
    json frontend_schema;
    bool retry_unsupported_parameter = true;
    std::function<void(const Channel &, const UpstreamRequest &, UpstreamPreparedRequest &)> prepare_upstream;
};

struct RegisteredRoute {
    RouteDescriptor descriptor;
    DataPlaneHandler *handler = nullptr;
};

class PluginRegistrar {
public:
    void register_data_plane_route(RouteDescriptor descriptor, DataPlaneHandler &handler);
    void register_channel_type(ChannelTypeDescriptor descriptor);
    void register_migrations(std::vector<std::string> migrations);

    const std::vector<RegisteredRoute> &routes() const
    {
        return routes_;
    }
    const std::vector<ChannelTypeDescriptor> &channel_types() const
    {
        return channel_types_;
    }
    const std::vector<std::string> &migrations() const
    {
        return migrations_;
    }

private:
    std::vector<RegisteredRoute> routes_;
    std::vector<ChannelTypeDescriptor> channel_types_;
    std::vector<std::string> migrations_;
};

class Plugin {
public:
    virtual ~Plugin() = default;
    virtual void register_with(PluginRegistrar &registrar) = 0;
};

using CreatePlugin = Plugin *(*)();
using DestroyPlugin = void (*)(Plugin *);

} // namespace revlm::plugin::v1
