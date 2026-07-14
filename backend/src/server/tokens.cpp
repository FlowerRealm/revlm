#include "server/tokens.hpp"

#include "auth/crypto.hpp"
#include "models/models.hpp"
#include "runtime/runtime_workers.hpp"
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
#include <utility>
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

std::string sql_in_list(odb::database &db, const std::vector<std::string> &values)
{
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += sql_quote(db, values[i]);
    }
    return out;
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

std::optional<TokenModelMapping> row_to_token_model_mapping(const SqlResultRow &row)
{
    if (row.size() < 3 || !row[0].has_value()) {
        return std::nullopt;
    }
    TokenModelMapping mapping;
    mapping.id.token_id = std::stoll(*row[0]);
    mapping.id.input_model = row[1].value_or("");
    mapping.target_model = row[2].value_or("");
    return mapping;
}

bool prune_token_model_mappings_for_targets(odb::database &db, long long token_id,
                                            const std::vector<TokenModelTargetOption> &targets)
{
    if (targets.empty()) {
        sql_exec(db, "DELETE FROM token_model_mappings WHERE token_id=" + std::to_string(token_id));
        return true;
    }

    std::vector<std::string> allowed;
    allowed.reserve(targets.size());
    std::unordered_set<std::string> seen;
    for (const TokenModelTargetOption &target : targets) {
        const std::string public_id = trim_ascii(target.public_id);
        if (!public_id.empty() && seen.insert(public_id).second) {
            allowed.push_back(public_id);
        }
    }
    if (allowed.empty()) {
        sql_exec(db, "DELETE FROM token_model_mappings WHERE token_id=" + std::to_string(token_id));
        return true;
    }

    sql_exec(db, "DELETE FROM token_model_mappings WHERE token_id=" + std::to_string(token_id) +
                     " AND target_model NOT IN (" + sql_in_list(db, allowed) + ")");
    return true;
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

std::pair<std::string, bool> resolve_model_mapping(const TokenAuth &auth, std::string_view model)
{
    const std::string normalized = trim_ascii(model);
    if (normalized.empty() || auth.model_mappings.empty()) {
        return { normalized, false };
    }
    const auto it = auth.model_mappings.find(normalized);
    if (it == auth.model_mappings.end()) {
        return { normalized, false };
    }
    const std::string target = trim_ascii(it->second);
    if (target.empty() || target == normalized) {
        return { normalized, false };
    }
    return { target, true };
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
    sql_exec(db_, "DELETE FROM token_model_mappings WHERE token_id=" + std::to_string(token_id));
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
    const auto mapping_rows =
        sql_query_rows(db_, "SELECT token_id,input_model,target_model FROM token_model_mappings WHERE token_id=" +
                                std::to_string(auth.token_id) + " ORDER BY input_model ASC");
    t.commit();
    for (const SqlResultRow &row : mapping_rows) {
        if (auto mapping = row_to_token_model_mapping(row); mapping.has_value()) {
            const std::string input = trim_ascii(mapping->id.input_model);
            const std::string target = trim_ascii(mapping->target_model);
            if (!input.empty() && !target.empty()) {
                auth.model_mappings[input] = target;
            }
        }
    }
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
    notify_runtime_routing_invalidated();
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
    (void)prune_token_model_mappings_for_targets(db_, token_id, list_token_model_mapping_targets(token_id));
    t.commit();
    notify_runtime_routing_invalidated();
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

std::vector<TokenModelMapping> TokenStore::list_token_model_mappings(long long token_id)
{
    if (!positive_id_or(token_id)) {
        return {};
    }
    ScopedTransaction t(db_);
    const auto rows =
        sql_query_rows(db_, "SELECT token_id,input_model,target_model FROM token_model_mappings WHERE token_id=" +
                                std::to_string(token_id) + " ORDER BY input_model ASC");
    t.commit();
    std::vector<TokenModelMapping> out;
    out.reserve(rows.size());
    for (const SqlResultRow &row : rows) {
        if (auto mapping = row_to_token_model_mapping(row); mapping.has_value()) {
            out.push_back(std::move(*mapping));
        }
    }
    return out;
}

std::vector<TokenModelTargetOption> TokenStore::list_token_model_mapping_targets(long long token_id)
{
    if (!positive_id_or(token_id)) {
        return {};
    }
    const std::vector<std::string> groups = list_effective_token_channel_groups(token_id);
    if (groups.empty()) {
        return {};
    }

    bool allow_openai = false;
    bool allow_anthropic = false;
    ScopedTransaction t(db_);
    const auto type_rows = sql_query_rows(db_, "SELECT DISTINCT c.type FROM channels c "
                                               "JOIN channel_group_members cgm ON cgm.channel_id=c.id "
                                               "JOIN channel_groups cg ON cg.id=cgm.channel_group_id AND cg.status=1 "
                                               "JOIN token_channel_groups tcg ON tcg.channel_group_id=cg.id "
                                               "WHERE tcg.token_id=" +
                                                   std::to_string(token_id) + " AND c.status=1");
    t.commit();
    for (const auto &row : type_rows) {
        const int type = static_cast<int>(std::stoll(row[0].value_or("0")));
        if (type == 1 || type == 2) {
            allow_openai = true;
        }
        if (type == 4) {
            allow_anthropic = true;
        }
    }

    const std::vector<Model> &models = ModelManager::instance().models();
    std::vector<TokenModelTargetOption> out;
    out.reserve(models.size());
    for (const Model &m : models) {
        const std::string public_id = trim_ascii(m.name);
        if (public_id.empty()) {
            continue;
        }
        const std::string owned = trim_ascii(m.owned_by);
        if (owned == "openai") {
            if (!allow_openai) {
                continue;
            }
        } else if (owned == "anthropic") {
            if (!allow_anthropic) {
                continue;
            }
        } else if (!allow_openai && !allow_anthropic) {
            continue;
        }
        out.push_back(TokenModelTargetOption{
            .public_id = public_id,
            .group_name = groups.front(),
            .owned_by = owned,
        });
    }
    std::sort(out.begin(), out.end(), [](const TokenModelTargetOption &a, const TokenModelTargetOption &b) {
        return a.public_id < b.public_id;
    });
    return out;
}

bool TokenStore::replace_token_model_mappings(long long token_id, const std::vector<TokenModelMappingCreate> &mappings)
{
    if (!positive_id_or(token_id)) {
        return false;
    }
    const std::vector<TokenModelMappingCreate> normalized = normalize_token_model_mappings(mappings);

    ScopedTransaction t(db_);
    const bool exists =
        sql_query_one(db_, "SELECT id FROM user_tokens WHERE id=" + std::to_string(token_id) + " LIMIT 1 FOR UPDATE")
            .has_value();
    if (!exists) {
        return false;
    }

    if (!normalized.empty()) {
        const std::vector<TokenModelTargetOption> targets = list_token_model_mapping_targets(token_id);
        std::unordered_set<std::string> allowed;
        allowed.reserve(targets.size());
        for (const TokenModelTargetOption &target : targets) {
            const std::string public_id = trim_ascii(target.public_id);
            if (!public_id.empty()) {
                allowed.insert(public_id);
            }
        }
        for (const TokenModelMappingCreate &mapping : normalized) {
            if (!allowed.contains(mapping.target_model)) {
                throw std::invalid_argument("目标模型不可用: " + mapping.target_model);
            }
        }
    }

    sql_exec(db_, "DELETE FROM token_model_mappings WHERE token_id=" + std::to_string(token_id));
    for (const TokenModelMappingCreate &mapping : normalized) {
        sql_exec(db_, "INSERT INTO token_model_mappings(token_id, input_model, target_model) VALUES(" +
                          std::to_string(token_id) + ", " + sql_quote(db_, mapping.input_model) + ", " +
                          sql_quote(db_, mapping.target_model) + ")");
    }
    t.commit();
    return true;
}

bool TokenStore::prune_token_model_mappings(long long token_id)
{
    if (!positive_id_or(token_id)) {
        return false;
    }
    ScopedTransaction t(db_);
    bool result = prune_token_model_mappings_for_targets(db_, token_id, list_token_model_mapping_targets(token_id));
    t.commit();
    return result;
}

void prune_token_model_mappings_for_tokens(odb::database &db, const std::vector<long long> &token_ids)
{
    if (token_ids.empty()) {
        return;
    }
    for (long long token_id : token_ids) {
        const std::vector<std::string> groups = [&]() {
            ScopedTransaction t(db);
            const auto rows =
                sql_query_rows(db, "SELECT t.id,tcg.channel_group_id,cg.name,tcg.priority "
                                   "FROM user_tokens t "
                                   "JOIN token_channel_groups tcg ON tcg.token_id=t.id "
                                   "JOIN channel_groups cg ON cg.id=tcg.channel_group_id AND cg.status=1 "
                                   "WHERE t.id=" +
                                       std::to_string(token_id) + " ORDER BY tcg.priority DESC, cg.name ASC");
            t.commit();
            std::vector<std::string> out;
            std::unordered_set<std::string> seen;
            for (const SqlResultRow &row : rows) {
                if (row.size() >= 3 && row[2].has_value()) {
                    const std::string name = trim_ascii(*row[2]);
                    if (!name.empty() && seen.insert(name).second) {
                        out.push_back(name);
                    }
                }
            }
            return out;
        }();
        if (groups.empty()) {
            ScopedTransaction t(db);
            sql_exec(db, "DELETE FROM token_model_mappings WHERE token_id=" + std::to_string(token_id));
            t.commit();
            continue;
        }
        const std::vector<Model> &models = ModelManager::instance().models();
        std::vector<TokenModelTargetOption> targets;
        for (const Model &m : models) {
            const std::string public_id = trim_ascii(m.name);
            if (!public_id.empty()) {
                targets.push_back(TokenModelTargetOption{
                    .public_id = public_id,
                    .group_name = groups.front(),
                    .owned_by = trim_ascii(m.owned_by),
                });
            }
        }
        ScopedTransaction t(db);
        prune_token_model_mappings_for_targets(db, token_id, targets);
        t.commit();
    }
}

} // namespace revlm
