#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "channels/channels.hpp"
#include "config/config.hpp"
#include "server/http_server.hpp"
#include "store/mysql.hpp"

namespace revlm
{

class ChannelGroup {
public:
    ChannelGroup()
    {
    }
    ChannelGroup(long long id, std::string name, std::string description, double price_multiplier, int status = 1)
        : id(id)
        , name(name)
        , description(description)
        , price_multiplier(price_multiplier)
        , status(status)
    {
    }
    long long id = 0;
    std::string name;
    std::string description;
    double price_multiplier;
    int status = 1; // 1 means active, 0 means inactive
    std::vector<Channel> channels;
    int pointer = 0;

    void next_channel();
};

class ChannelGroupStore {
public:
    static ChannelGroupStore &instance();
    void reload(MysqlConnection &conn);

    std::vector<ChannelGroup> list_channel_groups();
    ChannelGroup get_channel_group_by_id(long long id);
    // return id, 0 means error
    int create_channel_group(std::string_view name, std::string_view description, double price_multiplier,
                             int status = 1);
    bool update_channel_group(long long id, std::string_view name, std::string_view description,
                              double price_multiplier);
    bool delete_channel_group(long long id);
    bool add_channel_group_member(long long id, Channel channel);
    bool remove_channel_group_member(long long id, long long channel_id);
    bool create_channel_group_member(long long id, std::vector<Channel> channels);

private:
    ChannelGroupStore() = default;
    void load_from_db(MysqlConnection &conn);

    MysqlConnection *conn_ = nullptr;
    std::vector<ChannelGroup> channel_groups;
    long long id = 0;
    bool align_group_channels_to_db(long long id);
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
