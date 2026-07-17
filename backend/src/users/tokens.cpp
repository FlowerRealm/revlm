#include "users/tokens.hpp"

#include "auth/crypto.hpp"
#include "store/database.hpp"
#include "revlm_entities-odb.hxx"

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <cassert>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

bool positive_id_or(long long value)
{
    assert(value > 0 && "internal: id must be positive");
    return value > 0;
}

odb::nullable<std::string> nullable_trimmed(const odb::nullable<std::string> &value)
{
    if (value.null()) {
        return {};
    }
    std::string trimmed = trim_ascii(*value);
    if (trimmed.empty()) {
        return {};
    }
    odb::nullable<std::string> out;
    out = std::move(trimmed);
    return out;
}

std::optional<UserToken> row_to_user_token(const SqlResultRow &row)
{
    if (row.size() < 7 || !row[0].has_value()) {
        return std::nullopt;
    }
    UserToken token;
    token.id = std::stoll(*row[0]);
    token.user_id = std::stoll(row[1].value_or("0"));
    if (row[2].has_value()) {
        token.name = *row[2];
    }
    token.token_hash = row[3].value_or("");
    if (row[4].has_value()) {
        token.token_plain = *row[4];
    }
    token.status = std::stoi(row[5].value_or("0"));
    token.channel_group_id = std::stoll(row[6].value_or("0"));
    return token;
}

} // namespace

std::string new_random_token(std::string_view prefix, int bytes_len)
{
    if (bytes_len < 16) {
        bytes_len = 16;
    }
    return std::string{ prefix } + base64url_encode(random_bytes(static_cast<size_t>(bytes_len)));
}

std::string token_hash(std::string_view raw_token)
{
    return hex_encode(sha256_bytes(raw_token));
}

TokenStore::TokenStore()
    : db_(database())
    , requests_()
{
}

RequestStore &TokenStore::requests()
{
    return requests_;
}

long long TokenStore::create_user_token(long long user_id, const odb::nullable<std::string> &name,
                                        std::string_view raw_token)
{
    if (!positive_id_or(user_id)) {
        return 0;
    }
    if (raw_token.empty()) {
        throw std::invalid_argument("raw token must not be empty");
    }
    const auto clean_name = nullable_trimmed(name);
    const std::string hash = token_hash(raw_token);

    ScopedTransaction t(db_);
    sql_exec(db_, "INSERT INTO user_tokens(user_id, name, token_hash, token_plain, status, channel_group_id) VALUES(" +
                      std::to_string(user_id) + ", " +
                      (clean_name.null() ? std::string("NULL") : sql_quote(db_, *clean_name)) + ", " +
                      sql_quote(db_, hash) + ", " + sql_quote(db_, raw_token) + ", 1, 0)");
    const auto id_str = sql_query_one(db_, "SELECT LAST_INSERT_ID()");
    t.commit();
    return id_str ? std::stoll(*id_str) : 0;
}

std::vector<UserToken> TokenStore::list_user_tokens(long long user_id)
{
    if (!positive_id_or(user_id)) {
        return {};
    }
    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(
        db_,
        "SELECT id,user_id,name,token_hash,NULL AS token_plain,status,channel_group_id FROM user_tokens WHERE user_id=" +
            std::to_string(user_id) + " ORDER BY id DESC");
    t.commit();
    std::vector<UserToken> out;
    out.reserve(rows.size());
    for (const SqlResultRow &row : rows) {
        if (auto token = row_to_user_token(row); token.has_value()) {
            out.push_back(std::move(*token));
        }
    }
    return out;
}

std::optional<UserToken> TokenStore::get_user_token_by_id(long long user_id, long long token_id)
{
    if (!positive_id_or(user_id) || !positive_id_or(token_id)) {
        return std::nullopt;
    }
    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(
        db_, "SELECT id,user_id,name,token_hash,token_plain,status,channel_group_id FROM user_tokens WHERE id=" +
                 std::to_string(token_id) + " AND user_id=" + std::to_string(user_id));
    t.commit();
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_user_token(rows[0]);
}

std::optional<std::string> TokenStore::reveal_user_token(long long user_id, long long token_id)
{
    if (!positive_id_or(user_id) || !positive_id_or(token_id)) {
        return std::nullopt;
    }
    ScopedTransaction t(db_);
    const auto got =
        sql_query_one(db_, "SELECT token_plain FROM user_tokens WHERE id=" + std::to_string(token_id) +
                               " AND user_id=" + std::to_string(user_id) + " AND status=1 AND token_plain IS NOT NULL");
    t.commit();
    if (!got.has_value() || got->empty()) {
        return std::nullopt;
    }
    return got;
}

bool TokenStore::rotate_user_token(long long user_id, long long token_id, std::string_view raw_token)
{
    if (!positive_id_or(user_id) || !positive_id_or(token_id)) {
        return false;
    }
    if (raw_token.empty()) {
        throw std::invalid_argument("raw token must not be empty");
    }
    const std::string hash = token_hash(raw_token);
    ScopedTransaction t(db_);
    const bool owned = sql_query_one(db_, "SELECT id FROM user_tokens WHERE id=" + std::to_string(token_id) +
                                              " AND user_id=" + std::to_string(user_id) + " LIMIT 1 FOR UPDATE")
                           .has_value();
    if (!owned) {
        return false;
    }
    sql_exec(db_, "UPDATE user_tokens SET token_hash=" + sql_quote(db_, hash) +
                      ", token_plain=" + sql_quote(db_, raw_token) + ", status=1 WHERE id=" + std::to_string(token_id) +
                      " AND user_id=" + std::to_string(user_id));
    t.commit();
    return true;
}

void TokenStore::revoke_user_token(long long user_id, long long token_id)
{
    if (!positive_id_or(user_id) || !positive_id_or(token_id)) {
        return;
    }
    ScopedTransaction t(db_);
    sql_exec(db_, "UPDATE user_tokens SET status=0, token_plain=NULL WHERE id=" + std::to_string(token_id) +
                      " AND user_id=" + std::to_string(user_id) + " AND status=1");
    t.commit();
}

bool TokenStore::delete_user_token(long long user_id, long long token_id)
{
    if (!positive_id_or(user_id) || !positive_id_or(token_id)) {
        return false;
    }
    ScopedTransaction t(db_);
    const bool owned = sql_query_one(db_, "SELECT id FROM user_tokens WHERE id=" + std::to_string(token_id) +
                                              " AND user_id=" + std::to_string(user_id) + " LIMIT 1 FOR UPDATE")
                           .has_value();
    if (!owned) {
        return false;
    }
    sql_exec(db_, "DELETE FROM request_totals WHERE token_id=" + std::to_string(token_id));
    sql_exec(db_, "DELETE FROM requests WHERE token_id=" + std::to_string(token_id));
    sql_exec(db_, "DELETE FROM user_tokens WHERE id=" + std::to_string(token_id) +
                      " AND user_id=" + std::to_string(user_id));
    t.commit();
    return true;
}

std::optional<long long> TokenStore::resolve_token_channel_group_by_raw_token(std::string_view raw_token,
                                                                              long long &user_id, long long &token_id)
{
    if (raw_token.empty()) {
        return std::nullopt;
    }
    const std::string hash = token_hash(raw_token);
    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(db_, "SELECT u.id,t.id,t.channel_group_id FROM user_tokens t "
                                          "JOIN users u ON u.id=t.user_id "
                                          "WHERE t.token_hash=" +
                                              sql_quote(db_, hash) + " AND t.status=1 AND u.status=1 LIMIT 1");
    t.commit();
    if (rows.empty() || rows[0].size() < 3 || !rows[0][0].has_value()) {
        return std::nullopt;
    }
    user_id = std::stoll(*rows[0][0]);
    token_id = std::stoll(rows[0][1].value_or("0"));
    const long long channel_group_id = std::stoll(rows[0][2].value_or("0"));
    if (channel_group_id <= 0) {
        return std::nullopt;
    }
    return channel_group_id;
}

bool TokenStore::set_token_channel_group(long long user_id, long long token_id, long long channel_group_id)
{
    if (!positive_id_or(user_id) || !positive_id_or(token_id) || !positive_id_or(channel_group_id)) {
        return false;
    }
    ScopedTransaction t(db_);
    const bool owned = sql_query_one(db_, "SELECT id FROM user_tokens WHERE id=" + std::to_string(token_id) +
                                              " AND user_id=" + std::to_string(user_id) + " LIMIT 1 FOR UPDATE")
                           .has_value();
    if (!owned) {
        return false;
    }
    const auto group_status = sql_query_one(
        db_, "SELECT status FROM channel_groups WHERE id=" + std::to_string(channel_group_id) + " LIMIT 1 FOR UPDATE");
    if (!group_status.has_value()) {
        throw std::invalid_argument("渠道组不存在");
    }
    if (std::stoi(*group_status) == 0) {
        throw std::invalid_argument("渠道组已禁用");
    }
    sql_exec(db_, "UPDATE user_tokens SET channel_group_id=" + std::to_string(channel_group_id) +
                      " WHERE id=" + std::to_string(token_id) + " AND user_id=" + std::to_string(user_id));
    t.commit();
    return true;
}

} // namespace revlm
