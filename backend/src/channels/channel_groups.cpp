#include "channels/channel_groups.hpp"

#include "store/database.hpp"
#include "revlm_entities-odb.hxx"

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace revlm
{

void ChannelGroup::next_channel()
{
    pointer = (pointer + 1) % static_cast<int>(channels.size() + channels.empty());
}

ChannelGroupStore::ChannelGroupStore(odb::database &db)
    : db_(db)
{
}

void ChannelGroupStore::fill_channels(ChannelGroup &g)
{
    g.channels.clear();
    for (long long cid : g.channel_ids) {
        auto ch = db_.find<Channel>(cid);
        if (ch) {
            g.channels.push_back(*ch);
        }
    }
}

std::vector<ChannelGroup> ChannelGroupStore::list_channel_groups()
{
    ScopedTransaction t(db_);
    const auto group_rows =
        sql_query_rows(db_, "SELECT id,name,description,price_multiplier,status FROM channel_groups ORDER BY id");
    if (group_rows.empty()) {
        t.commit();
        return {};
    }

    std::vector<ChannelGroup> groups;
    std::string ids;
    for (size_t i = 0; i < group_rows.size(); ++i) {
        const auto &row = group_rows[i];
        groups.push_back(ChannelGroup(std::stoll(row[0].value_or("0")), row[1].value_or(""), row[2].value_or(""),
                                      std::stod(row[3].value_or("1")), std::stoi(row[4].value_or("0"))));
        if (i) {
            ids += ",";
        }
        ids += std::to_string(groups.back().id);
    }

    std::unordered_map<long long, std::vector<Channel>> by_group;
    const auto member_rows =
        sql_query_rows(db_, "SELECT m.channel_group_id,c.id,c.type,c.name,c.status,c.priority,c.base_url "
                            "FROM channel_group_members m "
                            "JOIN channels c ON c.id=m.channel_id "
                            "WHERE m.channel_group_id IN (" +
                                ids + ") ORDER BY m.channel_group_id, m.channel_id");
    for (const auto &row : member_rows) {
        by_group[std::stoll(row[0].value_or("0"))].push_back(
            Channel(std::stoll(row[1].value_or("0")), std::stoi(row[2].value_or("0")), row[3].value_or(""),
                    std::stoi(row[4].value_or("0")), std::stoi(row[5].value_or("0")), row[6].value_or("")));
    }
    for (ChannelGroup &g : groups) {
        if (auto it = by_group.find(g.id); it != by_group.end()) {
            g.channels = std::move(it->second);
        }
    }
    t.commit();
    return groups;
}

ChannelGroup ChannelGroupStore::get_channel_group_by_id(long long id)
{
    ScopedTransaction t(db_);
    auto p = db_.find<ChannelGroup>(id);
    if (!p) {
        t.commit();
        return {};
    }
    ChannelGroup g = *p;
    fill_channels(g);
    t.commit();
    return g;
}

int ChannelGroupStore::create_channel_group(std::string_view name, std::string_view description,
                                            double price_multiplier, int status)
{
    ChannelGroup g;
    g.name = std::string{ name };
    g.description = std::string{ description };
    g.price_multiplier = price_multiplier;
    g.status = status;
    ScopedTransaction t(db_);
    db_.persist(g);
    t.commit();
    return static_cast<int>(g.id);
}

bool ChannelGroupStore::update_channel_group(long long id, std::string_view name, std::string_view description,
                                             double price_multiplier)
{
    ScopedTransaction t(db_);
    auto p = db_.find<ChannelGroup>(id);
    if (!p) {
        return false;
    }
    p->name = std::string{ name };
    p->description = std::string{ description };
    p->price_multiplier = price_multiplier;
    db_.update(*p);
    t.commit();
    return true;
}

bool ChannelGroupStore::delete_channel_group(long long id)
{
    ScopedTransaction t(db_);
    auto p = db_.find<ChannelGroup>(id);
    if (!p) {
        return false;
    }
    sql_exec(db_, "DELETE FROM token_channel_groups WHERE channel_group_id=" + std::to_string(id));
    db_.erase(*p);
    t.commit();
    return true;
}

bool ChannelGroupStore::add_channel_group_member(long long id, Channel channel)
{
    ScopedTransaction t(db_);
    auto p = db_.find<ChannelGroup>(id);
    if (!p) {
        return false;
    }
    p->channel_ids.push_back(channel.id);
    db_.update(*p);
    t.commit();
    return true;
}

bool ChannelGroupStore::remove_channel_group_member(long long id, long long channel_id)
{
    ScopedTransaction t(db_);
    auto p = db_.find<ChannelGroup>(id);
    if (!p) {
        return false;
    }
    auto &ids = p->channel_ids;
    ids.erase(std::remove(ids.begin(), ids.end(), channel_id), ids.end());
    db_.update(*p);
    t.commit();
    return true;
}

bool ChannelGroupStore::create_channel_group_member(long long id, std::vector<Channel> channels)
{
    ScopedTransaction t(db_);
    auto p = db_.find<ChannelGroup>(id);
    if (!p) {
        return false;
    }
    p->channel_ids.clear();
    for (const Channel &ch : channels) {
        p->channel_ids.push_back(ch.id);
    }
    db_.update(*p);
    t.commit();
    return true;
}

} // namespace revlm
