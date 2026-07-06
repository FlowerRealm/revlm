#include "channels/channels.hpp"
#include "runtime/runtime_workers.hpp"
#include "server/tokens.hpp"

#include <soci/soci.h>

namespace revlm
{

ChannelStore::ChannelStore(MysqlConnection &conn)
    : conn_(conn)
{
}

std::vector<Channel> ChannelStore::list_channels()
{
    const auto rows = conn_.query_rows("SELECT id, type, name, status, priority, base_url, api_key "
                                       "FROM channels "
                                       "ORDER BY priority DESC, id DESC");
    std::vector<Channel> out;
    for (const MysqlResultRow &row : rows)
        out.push_back(Channel(std::stoll(row[0].value_or("0")), std::stoi(row[1].value_or("0")), row[2].value_or(""),
                              std::stoi(row[3].value_or("0")) != 0, std::stoi(row[4].value_or("0")),
                              row[5].value_or(""), row[6].value_or("")));
    return out;
}

bool ChannelStore::create_channel(Channel &channel)
{
    DbTransaction tr(conn_);
    const int status = channel.status ? 1 : 0;
    conn_.session() << "INSERT INTO channels(type, name, status, priority, base_url, api_key) "
                       "VALUES(:type, :name, :status, :priority, :base_url, :api_key)",
        soci::use(channel.type), soci::use(channel.name), soci::use(status), soci::use(channel.priority),
        soci::use(channel.base_url), soci::use(channel.api_key);
    channel.id = static_cast<long long>(conn_.last_insert_id());
    tr.commit();
    notify_runtime_routing_invalidated();
    return true;
}

bool ChannelStore::update_channel(Channel &channel)
{
    std::vector<long long> token_ids;
    {
        DbTransaction tr(conn_);
        int old_status = 0;
        soci::indicator found = soci::i_null;
        conn_.session() << "SELECT status FROM channels WHERE id = :id LIMIT 1 FOR UPDATE", soci::use(channel.id),
            soci::into(old_status, found);
        if (found == soci::i_null)
            return false;

        const bool was_enabled = old_status != 0;
        const int new_status = channel.status ? 1 : 0;
        conn_.session() << "UPDATE channels SET name = :name, status = :status, priority = :priority, "
                           "base_url = :base_url, api_key = :api_key WHERE id = :id",
            soci::use(channel.name), soci::use(new_status), soci::use(channel.priority), soci::use(channel.base_url),
            soci::use(channel.api_key), soci::use(channel.id);

        if (was_enabled && !channel.status) {
            soci::rowset<soci::row> rs = (conn_.session().prepare << "SELECT DISTINCT tcg.token_id "
                                                                     "FROM token_channel_groups tcg "
                                                                     "JOIN channel_group_members m "
                                                                     "ON m.channel_group_id = tcg.channel_group_id "
                                                                     "WHERE m.channel_id = :id",
                                          soci::use(channel.id));
            for (const soci::row &row : rs)
                token_ids.push_back(row.get<long long>(0));
        }
        tr.commit();
    }
    prune_token_model_mappings_for_tokens(conn_, token_ids);
    notify_runtime_routing_invalidated();
    return true;
}

bool ChannelStore::delete_channel(Channel &channel)
{
    std::vector<long long> token_ids;
    {
        DbTransaction tr(conn_);
        long long locked_id = 0;
        soci::indicator found = soci::i_null;
        conn_.session() << "SELECT id FROM channels WHERE id = :id LIMIT 1 FOR UPDATE", soci::use(channel.id),
            soci::into(locked_id, found);
        if (found == soci::i_null)
            return false;

        soci::rowset<soci::row> rs = (conn_.session().prepare << "SELECT DISTINCT tcg.token_id "
                                                                 "FROM token_channel_groups tcg "
                                                                 "JOIN channel_group_members m "
                                                                 "ON m.channel_group_id = tcg.channel_group_id "
                                                                 "WHERE m.channel_id = :id",
                                      soci::use(channel.id));
        for (const soci::row &row : rs)
            token_ids.push_back(row.get<long long>(0));

        conn_.exec("DELETE FROM channel_group_members WHERE channel_id=" + std::to_string(channel.id) + ";" +
                   "DELETE FROM channels WHERE id=" + std::to_string(channel.id));
        tr.commit();
    }
    prune_token_model_mappings_for_tokens(conn_, token_ids);
    notify_runtime_routing_invalidated();
    return true;
}

} // namespace revlm
