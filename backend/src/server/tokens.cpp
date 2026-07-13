#include "server/tokens.hpp"

#include "auth/crypto.hpp"
#include "auth/users.hpp"
#include "models/models.hpp"
#include "runtime/runtime_workers.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
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

std::optional<std::string> nullable_trimmed(std::optional<std::string> value)
{
    if (!value.has_value()) {
        return std::nullopt;
    }
    std::string trimmed = trim_ascii(*value);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    return trimmed;
}

void delete_app_setting_value(MysqlConnection &conn, std::string_view key, std::string_view value)
{
    conn.exec("DELETE FROM app_settings WHERE `key`=" + conn.quote(key) + " AND value=" + conn.quote(value));
}

std::string sql_in_list(MysqlConnection &conn, const std::vector<std::string> &values)
{
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += conn.quote(values[i]);
    }
    return out;
}

std::optional<UserToken> row_to_user_token(const MysqlResultRow &row)
{
    if (row.size() < 6 || !row[0].has_value()) {
        return std::nullopt;
    }
    UserToken token;
    token.id = std::stoll(*row[0]);
    token.user_id = std::stoll(row[1].value_or("0"));
    token.name = row[2];
    token.token_hash = row[3].value_or("");
    token.token_plain = row[4];
    token.status = std::stoi(row[5].value_or("0"));
    return token;
}

std::optional<ChannelGroup> row_to_channel_group(const MysqlResultRow &row)
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

std::optional<TokenChannelGroupBinding> row_to_token_channel_group_binding(const MysqlResultRow &row)
{
    if (row.size() < 4 || !row[0].has_value()) {
        return std::nullopt;
    }
    TokenChannelGroupBinding binding;
    binding.token_id = std::stoll(*row[0]);
    binding.channel_group_id = std::stoll(row[1].value_or("0"));
    binding.channel_group_name = row[2].value_or("");
    binding.priority = std::stoi(row[3].value_or("0"));
    return binding;
}

std::optional<TokenModelMapping> row_to_token_model_mapping(const MysqlResultRow &row)
{
    if (row.size() < 3 || !row[0].has_value()) {
        return std::nullopt;
    }
    TokenModelMapping mapping;
    mapping.token_id = std::stoll(*row[0]);
    mapping.input_model = row[1].value_or("");
    mapping.target_model = row[2].value_or("");
    return mapping;
}

bool prune_token_model_mappings_for_targets(MysqlConnection &conn, long long token_id,
                                            const std::vector<TokenModelTargetOption> &targets)
{
    if (targets.empty()) {
        conn.exec("DELETE FROM token_model_mappings WHERE token_id=" + std::to_string(token_id));
        return conn.affected_rows() > 0;
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
        conn.exec("DELETE FROM token_model_mappings WHERE token_id=" + std::to_string(token_id));
        return conn.affected_rows() > 0;
    }

    conn.exec("DELETE FROM token_model_mappings WHERE token_id=" + std::to_string(token_id) +
              " AND target_model NOT IN (" + sql_in_list(conn, allowed) + ")");
    return conn.affected_rows() > 0;
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
    return sha256_bytes(raw_token);
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

void TokenStore::reload(MysqlConnection &conn)
{
    conn_ = &conn;
    load_slots_from_db(conn);
}

void TokenStore::load_slots_from_db(MysqlConnection &conn)
{
    slots_.clear();
    const auto rows =
        conn.query_rows("SELECT id,user_id,name,token_hash,token_plain,status FROM user_tokens ORDER BY id");
    slots_.reserve(rows.size());
    for (const MysqlResultRow &row : rows) {
        auto token = row_to_user_token(row);
        if (!token.has_value()) {
            continue;
        }
        TokenSlot slot;
        slot.token = std::move(*token);
        slot.requests.bind(slot.token.user_id, slot.token.id);
        slot.requests.reload(conn);
        slots_.push_back(std::move(slot));
    }
}

TokenStore::TokenSlot *TokenStore::find_slot(long long token_id)
{
    for (TokenSlot &slot : slots_) {
        if (slot.token.id == token_id) {
            return &slot;
        }
    }
    return nullptr;
}

const TokenStore::TokenSlot *TokenStore::find_slot(long long token_id) const
{
    for (const TokenSlot &slot : slots_) {
        if (slot.token.id == token_id) {
            return &slot;
        }
    }
    return nullptr;
}

RequestStore &TokenStore::requests(long long token_id)
{
    if (TokenSlot *slot = find_slot(token_id)) {
        return slot->requests;
    }
    unbound_.bind(0, 0);
    if (conn_ != nullptr) {
        unbound_.reload(*conn_);
    }
    return unbound_;
}

long long TokenStore::create_user_token(long long user_id, const std::optional<std::string> &name,
                                        std::string_view raw_token)
{
    if (!positive_id_or(user_id)) {
        return 0;
    }
    if (raw_token.empty()) {
        throw std::invalid_argument("raw token must not be empty");
    }
    const std::optional<std::string> clean_name = nullable_trimmed(name);
    const std::string hash = token_hash(raw_token);
    conn_->exec("INSERT INTO user_tokens(user_id, name, token_hash, token_plain, status) VALUES(" +
                std::to_string(user_id) + ", " + sql_nullable(*conn_, clean_name) + ", " + conn_->quote(hash) + ", " +
                conn_->quote(raw_token) + ", 1)");
    const long long token_id = static_cast<long long>(conn_->last_insert_id());
    TokenSlot slot;
    slot.token.id = token_id;
    slot.token.user_id = user_id;
    slot.token.name = clean_name;
    slot.token.token_hash = hash;
    slot.token.token_plain = std::string{ raw_token };
    slot.token.status = 1;
    slot.requests.bind(user_id, token_id);
    if (conn_ != nullptr) {
        slot.requests.reload(*conn_);
    }
    slots_.push_back(std::move(slot));
    return token_id;
}

std::vector<UserToken> TokenStore::list_user_tokens(long long user_id)
{
    if (!positive_id_or(user_id)) {
        return {};
    }
    const auto rows = conn_->query_rows(
        "SELECT id,user_id,name,token_hash,NULL AS token_plain,status FROM user_tokens WHERE user_id=" +
        std::to_string(user_id) + " ORDER BY id DESC");
    std::vector<UserToken> out;
    out.reserve(rows.size());
    for (const MysqlResultRow &row : rows) {
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
    const auto rows = conn_->query_rows(
        "SELECT id,user_id,name,token_hash,token_plain,status FROM user_tokens WHERE id=" + std::to_string(token_id) +
        " AND user_id=" + std::to_string(user_id));
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
    const auto got =
        conn_->query_one("SELECT token_plain FROM user_tokens WHERE id=" + std::to_string(token_id) +
                         " AND user_id=" + std::to_string(user_id) + " AND status=1 AND token_plain IS NOT NULL");
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
    conn_->exec("UPDATE user_tokens SET token_hash=" + conn_->quote(hash) + ", token_plain=" + conn_->quote(raw_token) +
                ", status=1 WHERE id=" + std::to_string(token_id) + " AND user_id=" + std::to_string(user_id));
    const bool updated = conn_->affected_rows() > 0;
    return updated;
}

void TokenStore::revoke_user_token(long long user_id, long long token_id)
{
    if (!positive_id_or(user_id) || !positive_id_or(token_id)) {
        return;
    }
    conn_->exec("UPDATE user_tokens SET status=0, token_plain=NULL WHERE id=" + std::to_string(token_id) +
                " AND user_id=" + std::to_string(user_id) + " AND status=1");
}

bool TokenStore::delete_user_token(long long user_id, long long token_id)
{
    if (!positive_id_or(user_id) || !positive_id_or(token_id)) {
        return false;
    }
    DbTransaction tr(*conn_);
    const bool owned = conn_
                           ->query_one("SELECT id FROM user_tokens WHERE id=" + std::to_string(token_id) +
                                       " AND user_id=" + std::to_string(user_id) + " LIMIT 1 FOR UPDATE")
                           .has_value();
    if (!owned) {
        return false;
    }
    conn_->exec("DELETE FROM token_model_mappings WHERE token_id=" + std::to_string(token_id));
    conn_->exec("DELETE FROM token_channel_groups WHERE token_id=" + std::to_string(token_id));
    conn_->exec("DELETE FROM request_totals WHERE token_id=" + std::to_string(token_id));
    conn_->exec("DELETE FROM requests WHERE token_id=" + std::to_string(token_id));
    conn_->exec("DELETE FROM user_tokens WHERE id=" + std::to_string(token_id) +
                " AND user_id=" + std::to_string(user_id));
    const bool deleted = conn_->affected_rows() > 0;
    tr.commit();
    if (deleted) {
        std::erase_if(slots_, [token_id](const TokenSlot &slot) { return slot.token.id == token_id; });
    }
    return deleted;
}

std::optional<TokenAuth> TokenStore::get_token_auth_by_raw_token(std::string_view raw_token)
{
    if (raw_token.empty()) {
        return std::nullopt;
    }
    const std::string hash = token_hash(raw_token);
    const auto rows = conn_->query_rows("SELECT u.id,t.id,u.role,cg.name FROM user_tokens t "
                                        "JOIN users u ON u.id=t.user_id "
                                        "LEFT JOIN token_channel_groups tcg ON tcg.token_id=t.id "
                                        "LEFT JOIN channel_groups cg ON cg.id=tcg.channel_group_id AND cg.status=1 "
                                        "WHERE t.token_hash=" +
                                        conn_->quote(hash) +
                                        " AND t.status=1 AND u.status=1 "
                                        "ORDER BY tcg.priority DESC, cg.name ASC");
    if (rows.empty()) {
        return std::nullopt;
    }
    TokenAuth auth;
    std::unordered_set<std::string> seen_groups;
    bool found = false;
    for (const MysqlResultRow &row : rows) {
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
        return std::nullopt;
    }
    for (const TokenModelMapping &mapping : list_token_model_mappings(auth.token_id)) {
        const std::string input = trim_ascii(mapping.input_model);
        const std::string target = trim_ascii(mapping.target_model);
        if (!input.empty() && !target.empty()) {
            auth.model_mappings[input] = target;
        }
    }
    return auth;
}

std::vector<ChannelGroup> TokenStore::list_channel_groups()
{
    const auto rows = conn_->query_rows("SELECT id,name,description,price_multiplier,status FROM channel_groups "
                                        "ORDER BY status DESC, name ASC, id DESC");
    std::vector<ChannelGroup> out;
    out.reserve(rows.size());
    for (const MysqlResultRow &row : rows) {
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
    const auto rows = conn_->query_rows("SELECT id,name,description,price_multiplier,status "
                                        "FROM channel_groups WHERE id=" +
                                        std::to_string(group_id) + " LIMIT 1");
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_channel_group(rows[0]);
}

std::optional<ChannelGroup> TokenStore::get_channel_group_by_name(std::string_view name)
{
    const std::string normalized = normalize_channel_group_name(name);
    const auto rows = conn_->query_rows("SELECT id,name,description,price_multiplier,status "
                                        "FROM channel_groups WHERE name=" +
                                        conn_->quote(normalized) + " LIMIT 1");
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_channel_group(rows[0]);
}

std::optional<long long> TokenStore::get_default_channel_group_id()
{
    const auto raw = conn_->query_one("SELECT value FROM app_settings WHERE `key`=" +
                                      conn_->quote(default_channel_group_setting_key));
    if (!raw.has_value()) {
        return std::nullopt;
    }
    const auto id = parse_positive_i64_or(*raw);
    if (!id.has_value()) {
        delete_app_setting_value(*conn_, default_channel_group_setting_key, *raw);
        return std::nullopt;
    }
    const auto group = get_channel_group_by_id(*id);
    if (!group.has_value() || group->status != 1) {
        delete_app_setting_value(*conn_, default_channel_group_setting_key, *raw);
        return std::nullopt;
    }
    return id;
}

bool TokenStore::set_default_channel_group_id(long long group_id)
{
    if (!positive_id_or(group_id)) {
        return false;
    }
    DbTransaction tr(*conn_);
    const auto status = conn_->query_one("SELECT status FROM channel_groups WHERE id=" + std::to_string(group_id) +
                                         " LIMIT 1 FOR UPDATE");
    if (!status.has_value() || std::stoi(*status) != 1) {
        return false;
    }
    conn_->exec("INSERT INTO app_settings(`key`, value, created_at, updated_at) VALUES(" +
                conn_->quote(default_channel_group_setting_key) + ", " + conn_->quote(std::to_string(group_id)) +
                ", CURRENT_TIMESTAMP, CURRENT_TIMESTAMP) "
                "ON DUPLICATE KEY UPDATE value=VALUES(value), updated_at=CURRENT_TIMESTAMP");
    tr.commit();
    notify_runtime_routing_invalidated();
    return true;
}

std::vector<TokenChannelGroupBinding> TokenStore::list_token_channel_group_bindings(long long token_id)
{
    if (!positive_id_or(token_id)) {
        return {};
    }
    const auto rows = conn_->query_rows("SELECT tcg.token_id,tcg.channel_group_id,cg.name,tcg.priority "
                                        "FROM token_channel_groups tcg "
                                        "JOIN channel_groups cg ON cg.id=tcg.channel_group_id "
                                        "WHERE tcg.token_id=" +
                                        std::to_string(token_id) + " ORDER BY tcg.priority DESC, cg.name ASC");
    std::vector<TokenChannelGroupBinding> out;
    out.reserve(rows.size());
    for (const MysqlResultRow &row : rows) {
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

    DbTransaction tr(*conn_);
    const bool exists =
        conn_->query_one("SELECT id FROM user_tokens WHERE id=" + std::to_string(token_id) + " LIMIT 1 FOR UPDATE")
            .has_value();
    if (!exists) {
        return false;
    }

    std::unordered_set<long long> existing_group_ids;
    for (const auto &row : conn_->query_rows("SELECT channel_group_id FROM token_channel_groups WHERE token_id=" +
                                             std::to_string(token_id) + " FOR UPDATE")) {
        if (!row.empty() && row[0].has_value()) {
            existing_group_ids.insert(std::stoll(*row[0]));
        }
    }

    std::unordered_map<std::string, long long> id_by_name;
    id_by_name.reserve(normalized.size());
    for (const std::string &name : normalized) {
        const auto rows =
            conn_->query_rows("SELECT id FROM channel_groups WHERE name=" + conn_->quote(name) + " LIMIT 1");
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
    for (long long group_id : lock_ids) {
        const auto rows = conn_->query_rows("SELECT id,name,description,price_multiplier,status "
                                            "FROM channel_groups WHERE id=" +
                                            std::to_string(group_id) + " LIMIT 1 FOR UPDATE");
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

    conn_->exec("DELETE FROM token_channel_groups WHERE token_id=" + std::to_string(token_id));
    const int priority_base = static_cast<int>(groups.size()) * 10;
    for (size_t i = 0; i < groups.size(); ++i) {
        const int priority = priority_base - static_cast<int>(i) * 10;
        conn_->exec("INSERT INTO token_channel_groups(token_id, channel_group_id, priority, "
                    "created_at, updated_at) VALUES(" +
                    std::to_string(token_id) + ", " + std::to_string(groups[i].id) + ", " + std::to_string(priority) +
                    ", CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)");
    }
    (void)prune_token_model_mappings_for_targets(*conn_, token_id, list_token_model_mapping_targets(token_id));
    tr.commit();
    notify_runtime_routing_invalidated();
    return true;
}

std::vector<TokenChannelGroupBinding> TokenStore::list_effective_token_channel_group_bindings(long long token_id)
{
    if (!positive_id_or(token_id)) {
        return {};
    }
    const auto rows = conn_->query_rows("SELECT t.id,tcg.channel_group_id,cg.name,tcg.priority "
                                        "FROM user_tokens t "
                                        "JOIN token_channel_groups tcg ON tcg.token_id=t.id "
                                        "JOIN channel_groups cg ON cg.id=tcg.channel_group_id AND cg.status=1 "
                                        "WHERE t.id=" +
                                        std::to_string(token_id) + " ORDER BY tcg.priority DESC, cg.name ASC");
    std::vector<TokenChannelGroupBinding> out;
    out.reserve(rows.size());
    for (const MysqlResultRow &row : rows) {
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
    const auto rows =
        conn_->query_rows("SELECT token_id,input_model,target_model FROM token_model_mappings WHERE token_id=" +
                          std::to_string(token_id) + " ORDER BY input_model ASC");
    std::vector<TokenModelMapping> out;
    out.reserve(rows.size());
    for (const MysqlResultRow &row : rows) {
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
    const std::vector<Model> &models = ModelManager::instance().models();
    std::vector<TokenModelTargetOption> out;
    out.reserve(models.size());
    for (const Model &model : models) {
        const std::string public_id = trim_ascii(model.name);
        if (public_id.empty()) {
            continue;
        }
        out.push_back(TokenModelTargetOption{
            .public_id = public_id,
            .group_name = groups.front(),
            .owned_by = trim_ascii(model.owned_by),
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

    DbTransaction tr(*conn_);
    const bool exists =
        conn_->query_one("SELECT id FROM user_tokens WHERE id=" + std::to_string(token_id) + " LIMIT 1 FOR UPDATE")
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

    conn_->exec("DELETE FROM token_model_mappings WHERE token_id=" + std::to_string(token_id));
    for (const TokenModelMappingCreate &mapping : normalized) {
        conn_->exec("INSERT INTO token_model_mappings(token_id, input_model, target_model, "
                    "created_at, updated_at) VALUES(" +
                    std::to_string(token_id) + ", " + conn_->quote(mapping.input_model) + ", " +
                    conn_->quote(mapping.target_model) + ", CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)");
    }
    tr.commit();
    return true;
}

bool TokenStore::prune_token_model_mappings(long long token_id)
{
    if (!positive_id_or(token_id)) {
        return false;
    }
    return prune_token_model_mappings_for_targets(*conn_, token_id, list_token_model_mapping_targets(token_id));
}

void prune_token_model_mappings_for_tokens(MysqlConnection &conn, const std::vector<long long> &token_ids)
{
    if (token_ids.empty()) {
        return;
    }
    TokenStore &token_store = UserStore::instance().tokens();
    for (long long token_id : token_ids) {
        (void)token_store.prune_token_model_mappings(token_id);
    }
}

} // namespace revlm
