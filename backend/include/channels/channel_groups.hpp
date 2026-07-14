#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "channels/channels.hpp"
#include "config/config.hpp"
#include "server/http_server.hpp"

namespace odb
{
class database;
}

namespace revlm
{

#pragma db object table("channel_groups")
class ChannelGroup {
public:
    ChannelGroup()
    {
    }
    ChannelGroup(long long id, std::string name, std::string description, double price_multiplier, int status = 1)
        : id(id)
        , name(std::move(name))
        , description(std::move(description))
        , price_multiplier(price_multiplier)
        , status(status)
    {
    }

#pragma db id auto
    long long id = 0;
    std::string name;
    std::string description;
    double price_multiplier = 1.0;
    int status = 1;

#pragma db table("channel_group_members") id_column("channel_group_id") value_column("channel_id") unordered
    std::vector<long long> channel_ids;

#pragma db transient
    std::vector<Channel> channels;
#pragma db transient
    int pointer = 0;

    void next_channel();
};

class ChannelGroupStore {
public:
    explicit ChannelGroupStore(odb::database &db);

    std::vector<ChannelGroup> list_channel_groups();
    ChannelGroup get_channel_group_by_id(long long id);
    int create_channel_group(std::string_view name, std::string_view description, double price_multiplier,
                             int status = 1);
    bool update_channel_group(long long id, std::string_view name, std::string_view description,
                              double price_multiplier);
    bool delete_channel_group(long long id);
    bool add_channel_group_member(long long id, Channel channel);
    bool remove_channel_group_member(long long id, long long channel_id);
    bool create_channel_group_member(long long id, std::vector<Channel> channels);

private:
    void fill_channels(ChannelGroup &g);
    odb::database &db_;
};

struct ChannelGroupsAdminParsedRequest {
    std::string_view method;
    std::string_view path;
    std::string_view target;
};

HttpResponse channel_groups_admin_route(std::string_view raw_request, std::string_view body,
                                        const ChannelGroupsAdminParsedRequest &parsed, const Config &config,
                                        std::string_view request_id);

} // namespace revlm
