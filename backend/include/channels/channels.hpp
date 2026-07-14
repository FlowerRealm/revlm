#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
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
    Channel()
    {
    }
    Channel(long long id, int type, std::string name, bool status, int priority, std::string base_url,
            std::string api_key = {})
        : id(id)
        , type(type)
        , name(std::move(name))
        , status(status)
        , priority(priority)
        , base_url(std::move(base_url))
        , api_key(std::move(api_key))
    {
    }

#pragma db id auto
    long long id = 0;
    int type = 0;
    std::string name;
    bool status = true;
    int priority = 0;
    std::string base_url;
    std::string api_key;
};

class ChannelStore {
public:
    explicit ChannelStore(odb::database &db);
    std::vector<Channel> list_channels();
    bool create_channel(Channel &channel);
    bool update_channel(Channel &channel);
    bool delete_channel(Channel &channel);

private:
    odb::database &db_;
};

struct ChannelAdminParsedRequest {
    std::string_view method;
    std::string_view path;
    std::string_view target;
};

HttpResponse channel_admin_route(std::string_view raw_request, std::string_view body,
                                 const ChannelAdminParsedRequest &parsed, const Config &config,
                                 std::string_view request_id);

} // namespace revlm
