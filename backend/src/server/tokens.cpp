#include "server/tokens.hpp"

#include "auth/crypto.hpp"
#include "store/database.hpp"
#include "revlm_entities-odb.hxx"

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <algorithm>
#include <cassert>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "util/strings.hpp"
#include "util/user_input.hpp"

namespace revlm
{
namespace
{

constexpr std::string_view default_channel_group_setting_key = "default_channel_group_id";

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

void delete_app_setting_value(odb::database &db, std::string_view key, std::string_view value)
{
    sql_exec(db, "DELETE FROM app_settings WHERE `key`=" + sql_quote(db, key) + " AND value=" + sql_quote(db, value));
}

std::optional<UserToken> row_to_user_token(const SqlResultRow &row)
{
    if (row.size() < 6 || !row[0].has_value()) {
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
    return token;
}

std::optional<ChannelGroup> row_to_channel_group(const SqlResultRow &row)
{
    if (row.size() < 5 || !row[0].has_value()) {
        return std::nullopt;
    }
    ChannelGroup group;
    group.id = std::stoll(*row[0]);
    group.name = row[1].value_or("");
    group.description = row[2].value_or("");
    group.price_multiplier = std::stod(row[3].value_or("1"));
    group.status = std::stoi(row[4].value_or("0"));
    return group;
}

std::optional<TokenChannelGroupBinding> row_to_token_channel_group_binding(const SqlResultRow &row)
{
    if (row.size() < 4 || !row[0].has_value()) {
        return std::nullopt;
    }
    TokenChannelGroupBinding binding;
    binding.id.token_id = std::stoll(*row[0]);
    binding.id.channel_group_id = std::stoll(row[1].value_or("0"));
    binding.channel_group_name = row[2].value_or("");
    binding.priority = std::stoi(row[3].value_or("0"));
    return binding;
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

TokenStore::TokenStore(odb::database &db)
    : db_(db)
    , requests_(db)
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
    sql_exec(db_, "INSERT INTO user_tokens(user_id, name, token_hash, token_plain, status) VALUES(" +
                      std::to_string(user_id) + ", " +
                      (clean_name.null() ? std::string("NULL") : sql_quote(db_, *clean_name)) + ", " +
                      sql_quote(db_, hash) + ", " + sql_quote(db_, raw_token) + ", 1)");
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
        db_, "SELECT id,user_id,name,token_hash,NULL AS token_plain,status FROM user_tokens WHERE user_id=" +
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
    const auto rows =
        sql_query_rows(db_, "SELECT id,user_id,name,token_hash,token_plain,status FROM user_tokens WHERE id=" +
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
    sql_exec(db_, "DELETE FROM token_channel_groups WHERE token_id=" + std::to_string(token_id));
    sql_exec(db_, "DELETE FROM request_totals WHERE token_id=" + std::to_string(token_id));
    sql_exec(db_, "DELETE FROM requests WHERE token_id=" + std::to_string(token_id));
    sql_exec(db_, "DELETE FROM user_tokens WHERE id=" + std::to_string(token_id) +
                      " AND user_id=" + std::to_string(user_id));
    t.commit();
    return true;
}

std::optional<TokenAuth> TokenStore::get_token_auth_by_raw_token(std::string_view raw_token)
{
    if (raw_token.empty()) {
        return std::nullopt;
    }
    const std::string hash = token_hash(raw_token);
    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(db_, "SELECT u.id,t.id,u.role,cg.name FROM user_tokens t "
                                          "JOIN users u ON u.id=t.user_id "
                                          "LEFT JOIN token_channel_groups tcg ON tcg.token_id=t.id "
                                          "LEFT JOIN channel_groups cg ON cg.id=tcg.channel_group_id AND cg.status=1 "
                                          "WHERE t.token_hash=" +
                                              sql_quote(db_, hash) +
                                              " AND t.status=1 AND u.status=1 "
                                              "ORDER BY tcg.priority DESC, cg.name ASC");
    if (rows.empty()) {
        t.commit();
        return std::nullopt;
    }
    TokenAuth auth;
    std::unordered_set<std::string> seen_groups;
    bool found = false;
    for (const SqlResultRow &row : rows) {
        if (row.size() < 4 || !row[0].has_value()) {
            continue;
        }
        if (!found) {
            auth.user_id = std::stoll(*row[0]);
            auth.token_id = std::stoll(row[1].value_or("0"));
            auth.role = row[2].value_or("");
            found = true;
        }
        if (!row[3].has_value()) {
            continue;
        }
        const std::string name = trim_ascii(*row[3]);
        if (!name.empty() && seen_groups.insert(name).second) {
            auth.groups.push_back(name);
        }
    }
    if (!found) {
        t.commit();
        return std::nullopt;
    }
    t.commit();
    return auth;
}

std::vector<ChannelGroup> TokenStore::list_channel_groups()
{
    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(db_, "SELECT id,name,description,price_multiplier,status FROM channel_groups "
                                          "ORDER BY status DESC, name ASC, id DESC");
    t.commit();
    std::vector<ChannelGroup> out;
    out.reserve(rows.size());
    for (const SqlResultRow &row : rows) {
        if (auto group = row_to_channel_group(row); group.has_value()) {
            out.push_back(std::move(*group));
        }
    }
    return out;
}

std::optional<ChannelGroup> TokenStore::get_channel_group_by_id(long long group_id)
{
    if (!positive_id_or(group_id)) {
        return std::nullopt;
    }
    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(db_, "SELECT id,name,description,price_multiplier,status "
                                          "FROM channel_groups WHERE id=" +
                                              std::to_string(group_id) + " LIMIT 1");
    t.commit();
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_channel_group(rows[0]);
}

std::optional<ChannelGroup> TokenStore::get_channel_group_by_name(std::string_view name)
{
    const std::string normalized = normalize_channel_group_name(name);
    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(db_, "SELECT id,name,description,price_multiplier,status "
                                          "FROM channel_groups WHERE name=" +
                                              sql_quote(db_, normalized) + " LIMIT 1");
    t.commit();
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_channel_group(rows[0]);
}

std::optional<long long> TokenStore::get_default_channel_group_id()
{
    ScopedTransaction t(db_);
    const auto raw = sql_query_one(db_, "SELECT value FROM app_settings WHERE `key`=" +
                                            sql_quote(db_, default_channel_group_setting_key));
    if (!raw.has_value()) {
        t.commit();
        return std::nullopt;
    }
    const auto id = parse_positive_i64_or(*raw);
    if (!id.has_value()) {
        delete_app_setting_value(db_, default_channel_group_setting_key, *raw);
        t.commit();
        return std::nullopt;
    }
    const auto group_rows = sql_query_rows(db_, "SELECT id,name,description,price_multiplier,status "
                                                "FROM channel_groups WHERE id=" +
                                                    std::to_string(*id) + " LIMIT 1");
    if (group_rows.empty()) {
        delete_app_setting_value(db_, default_channel_group_setting_key, *raw);
        t.commit();
        return std::nullopt;
    }
    auto group = row_to_channel_group(group_rows[0]);
    if (!group.has_value() || group->status != 1) {
        delete_app_setting_value(db_, default_channel_group_setting_key, *raw);
        t.commit();
        return std::nullopt;
    }
    t.commit();
    return id;
}

bool TokenStore::set_default_channel_group_id(long long group_id)
{
    if (!positive_id_or(group_id)) {
        return false;
    }
    ScopedTransaction t(db_);
    const auto status = sql_query_one(db_, "SELECT status FROM channel_groups WHERE id=" + std::to_string(group_id) +
                                               " LIMIT 1 FOR UPDATE");
    if (!status.has_value() || std::stoi(*status) != 1) {
        return false;
    }
    sql_exec(db_, "INSERT INTO app_settings(`key`, value) VALUES(" + sql_quote(db_, default_channel_group_setting_key) +
                      ", " + sql_quote(db_, std::to_string(group_id)) +
                      ") ON DUPLICATE KEY UPDATE value=VALUES(value)");
    t.commit();
    return true;
}

std::vector<TokenChannelGroupBinding> TokenStore::list_token_channel_group_bindings(long long token_id)
{
    if (!positive_id_or(token_id)) {
        return {};
    }
    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(db_, "SELECT tcg.token_id,tcg.channel_group_id,cg.name,tcg.priority "
                                          "FROM token_channel_groups tcg "
                                          "JOIN channel_groups cg ON cg.id=tcg.channel_group_id "
                                          "WHERE tcg.token_id=" +
                                              std::to_string(token_id) + " ORDER BY tcg.priority DESC, cg.name ASC");
    t.commit();
    std::vector<TokenChannelGroupBinding> out;
    out.reserve(rows.size());
    for (const SqlResultRow &row : rows) {
        if (auto binding = row_to_token_channel_group_binding(row); binding.has_value()) {
            out.push_back(std::move(*binding));
        }
    }
    return out;
}

bool TokenStore::replace_token_channel_groups(long long token_id, const std::vector<std::string> &names)
{
    if (!positive_id_or(token_id)) {
        return false;
    }
    const std::vector<std::string> normalized = normalize_token_channel_groups(names);
    if (normalized.empty()) {
        throw std::invalid_argument("至少选择一个渠道组");
    }

    ScopedTransaction t(db_);
    const bool exists =
        sql_query_one(db_, "SELECT id FROM user_tokens WHERE id=" + std::to_string(token_id) + " LIMIT 1 FOR UPDATE")
            .has_value();
    if (!exists) {
        return false;
    }

    std::unordered_set<long long> existing_group_ids;
    for (const auto &row : sql_query_rows(db_, "SELECT channel_group_id FROM token_channel_groups WHERE token_id=" +
                                                   std::to_string(token_id) + " FOR UPDATE")) {
        if (!row.empty() && row[0].has_value()) {
            existing_group_ids.insert(std::stoll(*row[0]));
        }
    }

    std::unordered_map<std::string, long long> id_by_name;
    id_by_name.reserve(normalized.size());
    for (const std::string &name : normalized) {
        const auto rows =
            sql_query_rows(db_, "SELECT id FROM channel_groups WHERE name=" + sql_quote(db_, name) + " LIMIT 1");
        if (rows.empty() || rows[0].empty() || !rows[0][0].has_value()) {
            throw std::invalid_argument("渠道组不存在: " + name);
        }
        id_by_name.emplace(name, std::stoll(*rows[0][0]));
    }

    std::vector<long long> lock_ids;
    lock_ids.reserve(id_by_name.size());
    for (const auto &[_, group_id] : id_by_name) {
        lock_ids.push_back(group_id);
    }
    std::sort(lock_ids.begin(), lock_ids.end());
    lock_ids.erase(std::unique(lock_ids.begin(), lock_ids.end()), lock_ids.end());
    std::unordered_map<long long, ChannelGroup> locked_groups;
    locked_groups.reserve(lock_ids.size());
    for (long long gid : lock_ids) {
        const auto rows = sql_query_rows(db_, "SELECT id,name,description,price_multiplier,status "
                                              "FROM channel_groups WHERE id=" +
                                                  std::to_string(gid) + " LIMIT 1 FOR UPDATE");
        if (rows.empty()) {
            throw std::invalid_argument("渠道组不存在");
        }
        auto group = row_to_channel_group(rows[0]);
        if (!group.has_value()) {
            throw std::invalid_argument("渠道组不存在");
        }
        if (group->status != 1 && !existing_group_ids.contains(group->id)) {
            throw std::invalid_argument("渠道组已禁用: " + group->name);
        }
        locked_groups.emplace(group->id, std::move(*group));
    }

    std::vector<ChannelGroup> groups;
    groups.reserve(normalized.size());
    for (const std::string &name : normalized) {
        const auto id_it = id_by_name.find(name);
        if (id_it == id_by_name.end()) {
            throw std::invalid_argument("渠道组不存在: " + name);
        }
        auto group_it = locked_groups.find(id_it->second);
        if (group_it == locked_groups.end()) {
            throw std::invalid_argument("渠道组不存在: " + name);
        }
        groups.push_back(group_it->second);
    }

    sql_exec(db_, "DELETE FROM token_channel_groups WHERE token_id=" + std::to_string(token_id));
    const int priority_base = static_cast<int>(groups.size()) * 10;
    for (size_t i = 0; i < groups.size(); ++i) {
        const int priority = priority_base - static_cast<int>(i) * 10;
        sql_exec(db_, "INSERT INTO token_channel_groups(token_id, channel_group_id, priority) VALUES(" +
                          std::to_string(token_id) + ", " + std::to_string(groups[i].id) + ", " +
                          std::to_string(priority) + ")");
    }
    t.commit();
    return true;
}

std::vector<TokenChannelGroupBinding> TokenStore::list_effective_token_channel_group_bindings(long long token_id)
{
    if (!positive_id_or(token_id)) {
        return {};
    }
    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(db_, "SELECT t.id,tcg.channel_group_id,cg.name,tcg.priority "
                                          "FROM user_tokens t "
                                          "JOIN token_channel_groups tcg ON tcg.token_id=t.id "
                                          "JOIN channel_groups cg ON cg.id=tcg.channel_group_id AND cg.status=1 "
                                          "WHERE t.id=" +
                                              std::to_string(token_id) + " ORDER BY tcg.priority DESC, cg.name ASC");
    t.commit();
    std::vector<TokenChannelGroupBinding> out;
    out.reserve(rows.size());
    for (const SqlResultRow &row : rows) {
        if (auto binding = row_to_token_channel_group_binding(row); binding.has_value()) {
            out.push_back(std::move(*binding));
        }
    }
    return out;
}

std::vector<std::string> TokenStore::list_effective_token_channel_groups(long long token_id)
{
    const std::vector<TokenChannelGroupBinding> bindings = list_effective_token_channel_group_bindings(token_id);
    std::vector<std::string> out;
    out.reserve(bindings.size());
    std::unordered_set<std::string> seen;
    for (const TokenChannelGroupBinding &binding : bindings) {
        const std::string name = trim_ascii(binding.channel_group_name);
        if (!name.empty() && seen.insert(name).second) {
            out.push_back(name);
        }
    }
    return out;
}

} // namespace revlm
