#include "channels/channel_groups.hpp"

#include "runtime/runtime_workers.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace revlm
{

namespace
{

ChannelGroup *find_group(std::vector<ChannelGroup> &groups, long long id)
{
    for (ChannelGroup &group : groups)
        if (group.id == id)
            return &group;
    return nullptr;
}

std::string price_sql(double value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", value);
    return buf;
}

} // namespace

void ChannelGroup::next_channel()
{
    pointer = (pointer + 1) % static_cast<int>(channels.size() + channels.empty());
}

ChannelGroupStore &ChannelGroupStore::instance()
{
    static ChannelGroupStore store;
    return store;
}

void ChannelGroupStore::reload(MysqlConnection &conn)
{
    conn_ = &conn;
    load_from_db(conn);
}

void ChannelGroupStore::load_from_db(MysqlConnection &conn)
{
    channel_groups.clear();
    id = 0;

    if (auto max_id = conn.query_one("SELECT COALESCE(MAX(id),0) FROM channel_groups"))
        id = std::stoll(*max_id);

    for (const MysqlResultRow &row :
         conn.query_rows("SELECT id,name,description,price_multiplier,status FROM channel_groups ORDER BY id")) {
        channel_groups.push_back(ChannelGroup(std::stoll(row[0].value_or("0")), row[1].value_or(""),
                                              row[2].value_or(""), std::stod(row[3].value_or("1")),
                                              std::stoi(row[4].value_or("0"))));
    }

    if (channel_groups.empty())
        return;

    std::string ids;
    for (size_t i = 0; i < channel_groups.size(); ++i) {
        if (i)
            ids += ",";
        ids += std::to_string(channel_groups[i].id);
    }

    std::unordered_map<long long, std::vector<Channel>> by_group;
    for (const MysqlResultRow &row :
         conn.query_rows("SELECT m.channel_group_id,c.id,c.type,c.name,c.status,c.priority,c.base_url "
                         "FROM channel_group_members m "
                         "JOIN channels c ON c.id=m.channel_id "
                         "WHERE m.channel_group_id IN (" +
                         ids + ") ORDER BY m.channel_group_id,m.id"))
        by_group[std::stoll(row[0].value_or("0"))].push_back(
            Channel(std::stoll(row[1].value_or("0")), std::stoi(row[2].value_or("0")), row[3].value_or(""),
                    std::stoi(row[4].value_or("0")) != 0, std::stoi(row[5].value_or("0")), row[6].value_or("")));

    for (ChannelGroup &group : channel_groups)
        if (auto it = by_group.find(group.id); it != by_group.end())
            group.channels = std::move(it->second);
}

std::vector<ChannelGroup> ChannelGroupStore::list_channel_groups()
{
    return channel_groups;
}

ChannelGroup ChannelGroupStore::get_channel_group_by_id(long long id)
{
    if (ChannelGroup *group = find_group(channel_groups, id))
        return *group;
    return {};
}

int ChannelGroupStore::create_channel_group(std::string_view name, std::string_view description,
                                            double price_multiplier, int status)
{
    channel_groups.push_back(
        ChannelGroup(++id, std::string{ name }, std::string{ description }, price_multiplier, status));
    align_group_channels_to_db(channel_groups.back().id);
    return static_cast<int>(channel_groups.back().id);
}

bool ChannelGroupStore::update_channel_group(long long id, std::string_view name, std::string_view description,
                                             double price_multiplier)
{
    ChannelGroup *group = find_group(channel_groups, id);
    if (!group)
        return false;
    *group = ChannelGroup(group->id, std::string{ name }, std::string{ description }, price_multiplier, group->status);
    return align_group_channels_to_db(id);
}

bool ChannelGroupStore::delete_channel_group(long long id)
{
    std::erase_if(channel_groups, [id](const ChannelGroup &group) { return group.id == id; });
    return align_group_channels_to_db(id);
}

bool ChannelGroupStore::add_channel_group_member(long long id, Channel channel)
{
    ChannelGroup *group = find_group(channel_groups, id);
    if (!group)
        return false;
    group->channels.push_back(std::move(channel));
    return align_group_channels_to_db(id);
}

bool ChannelGroupStore::remove_channel_group_member(long long id, long long channel_id)
{
    ChannelGroup *group = find_group(channel_groups, id);
    if (!group)
        return false;
    auto &channels = group->channels;
    channels.erase(std::remove_if(channels.begin(), channels.end(),
                                  [channel_id](const Channel &channel) { return channel.id == channel_id; }),
                   channels.end());
    return align_group_channels_to_db(id);
}

bool ChannelGroupStore::create_channel_group_member(long long id, std::vector<Channel> channels)
{
    ChannelGroup *group = find_group(channel_groups, id);
    if (!group)
        return false;
    group->channels = std::move(channels);
    return align_group_channels_to_db(id);
}

bool ChannelGroupStore::align_group_channels_to_db(long long id)
{
    ChannelGroup *group = find_group(channel_groups, id);
    if (!group) {
        DbTransaction tr(*conn_);
        conn_->exec("DELETE FROM channel_group_members WHERE channel_group_id=" + std::to_string(id) + ";" +
                    "DELETE FROM token_channel_groups WHERE channel_group_id=" + std::to_string(id) + ";" +
                    "DELETE FROM channel_groups WHERE id=" + std::to_string(id));
        tr.commit();
        notify_runtime_routing_invalidated();
        return true;
    }

    const std::string price = price_sql(group->price_multiplier);
    const std::string desc = conn_->quote(group->description);

    DbTransaction tr(*conn_);
    conn_->exec("UPDATE channel_groups SET name=" + conn_->quote(group->name) + ",description=" + desc +
                ",price_multiplier=" + conn_->quote(price) + ",status=" + std::to_string(group->status) +
                " WHERE id=" + std::to_string(group->id));
    if (conn_->affected_rows() == 0) {
        conn_->exec("INSERT INTO channel_groups(id,name,description,price_multiplier,status) VALUES(" +
                    std::to_string(group->id) + "," + conn_->quote(group->name) + "," + desc + "," +
                    conn_->quote(price) + "," + std::to_string(group->status) + ")");
    }

    conn_->exec("DELETE FROM channel_group_members WHERE channel_group_id=" + std::to_string(group->id));
    for (const Channel &channel : group->channels) {
        conn_->exec("INSERT INTO channel_group_members(channel_group_id,channel_id) VALUES(" +
                    std::to_string(group->id) + "," + std::to_string(channel.id) + ")");
    }
    tr.commit();
    notify_runtime_routing_invalidated();
    return true;
}

} // namespace revlm
