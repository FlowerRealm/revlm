#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "models/models.hpp"
#include "server/http_server.hpp"

namespace odb
{
class database;
}

namespace revlm
{

#pragma db object table("channels")
class Channel {
public:
    Channel() = default;
    Channel(long long id, std::string type, std::string name, bool status, int priority, std::string base_url,
            std::string api_key = {}, double price_multiplier = 1.0);

    const Model *find_model(std::string_view model_name) const;

#pragma db id auto
    long long id = 0;
    std::string type;
    std::string name;
    bool status = true;
    int priority = 0;
    std::string base_url;
    std::string api_key;
    double price_multiplier = 1.0;
#pragma db transient
    std::vector<Model> models;
};

class ChannelStore {
public:
    static ChannelStore &instance();

    std::vector<Channel> list_channels();
    std::optional<Channel> find_channel(long long id);
    bool create_channel(Channel &channel);
    bool update_channel(Channel &channel);
    bool delete_channel(Channel &channel);

    ChannelStore(const ChannelStore &) = delete;
    ChannelStore &operator=(const ChannelStore &) = delete;

private:
    friend void reset_stores_for_test();
    ChannelStore();
    static void reset_instance();

    odb::database &db_;
};

struct ChannelParsedRequest {
    std::string_view method;
    std::string_view path;
    std::string_view target;
};

HttpResponse channel_route(std::string_view raw_request, std::string_view body, const ChannelParsedRequest &parsed,
                           std::string_view request_id);

} // namespace revlm
