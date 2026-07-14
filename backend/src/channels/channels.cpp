#include "channels/channels.hpp"

#include "runtime/runtime_workers.hpp"
#include "server/tokens.hpp"
#include "store/database.hpp"
#include "revlm_entities-odb.hxx"

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <string>

namespace revlm
{

ChannelStore::ChannelStore(odb::database &db)
    : db_(db)
{
}

std::vector<Channel> ChannelStore::list_channels()
{
    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(db_, "SELECT id, type, name, status, priority, base_url, api_key "
                                          "FROM channels "
                                          "ORDER BY priority DESC, id DESC");
    t.commit();
    std::vector<Channel> out;
    for (const SqlResultRow &row : rows) {
        out.push_back(Channel(std::stoll(row[0].value_or("0")), std::stoi(row[1].value_or("0")), row[2].value_or(""),
                              std::stoi(row[3].value_or("0")), std::stoi(row[4].value_or("0")), row[5].value_or(""),
                              row[6].value_or("")));
    }
    return out;
}

bool ChannelStore::create_channel(Channel &channel)
{
    ScopedTransaction t(db_);
    db_.persist(channel);
    t.commit();
    notify_runtime_routing_invalidated();
    return true;
}

bool ChannelStore::update_channel(Channel &channel)
{
    std::vector<long long> token_ids;
    {
        ScopedTransaction t(db_);
        const auto old_rows =
            sql_query_rows(db_, "SELECT status FROM channels WHERE id = " + std::to_string(channel.id) +
                                    " LIMIT 1 FOR UPDATE");
        if (old_rows.empty()) {
            return false;
        }
        const bool was_enabled = std::stoi(old_rows[0][0].value_or("0")) != 0;
        db_.update(channel);

        if (was_enabled && channel.status == 0) {
            const auto tkn_rows =
                sql_query_rows(db_, "SELECT DISTINCT tcg.token_id "
                                    "FROM token_channel_groups tcg "
                                    "JOIN channel_group_members m ON m.channel_group_id = tcg.channel_group_id "
                                    "WHERE m.channel_id = " +
                                        std::to_string(channel.id));
            for (const auto &row : tkn_rows) {
                if (!row.empty() && row[0]) {
                    token_ids.push_back(std::stoll(*row[0]));
                }
            }
        }
        t.commit();
    }
    prune_token_model_mappings_for_tokens(db_, token_ids);
    notify_runtime_routing_invalidated();
    return true;
}

bool ChannelStore::delete_channel(Channel &channel)
{
    std::vector<long long> token_ids;
    {
        ScopedTransaction t(db_);
        const auto check = sql_query_rows(
            db_, "SELECT id FROM channels WHERE id = " + std::to_string(channel.id) + " LIMIT 1 FOR UPDATE");
        if (check.empty()) {
            return false;
        }
        const auto tkn_rows =
            sql_query_rows(db_, "SELECT DISTINCT tcg.token_id "
                                "FROM token_channel_groups tcg "
                                "JOIN channel_group_members m ON m.channel_group_id = tcg.channel_group_id "
                                "WHERE m.channel_id = " +
                                    std::to_string(channel.id));
        for (const auto &row : tkn_rows) {
            if (!row.empty() && row[0]) {
                token_ids.push_back(std::stoll(*row[0]));
            }
        }
        sql_exec(db_, "DELETE FROM channel_group_members WHERE channel_id=" + std::to_string(channel.id));
        db_.erase(channel);
        t.commit();
    }
    prune_token_model_mappings_for_tokens(db_, token_ids);
    notify_runtime_routing_invalidated();
    return true;
}

} // namespace revlm
