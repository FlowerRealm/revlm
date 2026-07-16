#include "channels/channels.hpp"

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
    const auto rows = sql_query_rows(db_,
                                     "SELECT id, type, name, status, priority, base_url, api_key, price_multiplier "
                                     "FROM channels "
                                     "ORDER BY priority DESC, id DESC");
    t.commit();
    std::vector<Channel> out;
    for (const SqlResultRow &row : rows) {
        out.push_back(Channel(std::stoll(row[0].value_or("0")), std::stoi(row[1].value_or("0")), row[2].value_or(""),
                              std::stoi(row[3].value_or("0")) != 0, std::stoi(row[4].value_or("0")),
                              row[5].value_or(""), row[6].value_or(""), std::stod(row[7].value_or("1"))));
    }
    return out;
}

bool ChannelStore::create_channel(Channel &channel)
{
    ScopedTransaction t(db_);
    db_.persist(channel);
    t.commit();
    return true;
}

bool ChannelStore::update_channel(Channel &channel)
{
    ScopedTransaction t(db_);
    const auto old_rows = sql_query_rows(db_, "SELECT status FROM channels WHERE id = " + std::to_string(channel.id) +
                                                  " LIMIT 1 FOR UPDATE");
    if (old_rows.empty()) {
        return false;
    }
    db_.update(channel);
    t.commit();
    return true;
}

bool ChannelStore::delete_channel(Channel &channel)
{
    ScopedTransaction t(db_);
    const auto check =
        sql_query_rows(db_, "SELECT id FROM channels WHERE id = " + std::to_string(channel.id) + " LIMIT 1 FOR UPDATE");
    if (check.empty()) {
        return false;
    }
    sql_exec(db_, "DELETE FROM channel_group_members WHERE channel_id=" + std::to_string(channel.id));
    sql_exec(db_, "UPDATE user_tokens SET channel_id=0 WHERE channel_id=" + std::to_string(channel.id));
    db_.erase(channel);
    t.commit();
    return true;
}

} // namespace revlm
