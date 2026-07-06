#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "channels/channel_groups.hpp"
#include "store/mysql.hpp"

namespace revlm
{

struct UserToken {
    long long id = 0;
    long long user_id = 0;
    std::optional<std::string> name;
    std::string token_hash;
    std::optional<std::string> token_plain;
    int status = 0;
};

struct TokenAuth {
    long long user_id = 0;
    long long token_id = 0;
    std::string role;
    std::vector<std::string> groups;
    std::unordered_map<std::string, std::string> model_mappings;
};

struct TokenChannelGroupBinding {
    long long token_id = 0;
    long long channel_group_id = 0;
    std::string channel_group_name;
    int priority = 0;
};

struct TokenModelMapping {
    long long token_id = 0;
    std::string input_model;
    std::string target_model;
};

struct TokenModelMappingCreate {
    std::string input_model;
    std::string target_model;
};

struct TokenModelTargetOption {
    std::string public_id;
    std::string group_name;
    std::string owned_by;
};

std::string new_random_token(std::string_view prefix = "sk_", int bytes_len = 32);
std::string token_hash(std::string_view raw_token);
std::string hex_encode(std::string_view bytes);
std::pair<std::string, bool> resolve_model_mapping(const TokenAuth &auth, std::string_view model);
void prune_token_model_mappings_for_tokens(MysqlConnection &conn, const std::vector<long long> &token_ids);

class TokenStore {
public:
    explicit TokenStore(MysqlConnection &conn);

    long long create_user_token(long long user_id, const std::optional<std::string> &name, std::string_view raw_token);
    std::vector<UserToken> list_user_tokens(long long user_id);
    std::optional<UserToken> get_user_token_by_id(long long user_id, long long token_id);
    std::optional<std::string> reveal_user_token(long long user_id, long long token_id);
    bool rotate_user_token(long long user_id, long long token_id, std::string_view raw_token);
    void revoke_user_token(long long user_id, long long token_id);
    bool delete_user_token(long long user_id, long long token_id);
    std::optional<TokenAuth> get_token_auth_by_raw_token(std::string_view raw_token);

    std::vector<ChannelGroup> list_channel_groups();
    std::optional<ChannelGroup> get_channel_group_by_id(long long group_id);
    std::optional<ChannelGroup> get_channel_group_by_name(std::string_view name);
    std::optional<long long> get_default_channel_group_id();
    bool set_default_channel_group_id(long long group_id);

    std::vector<TokenChannelGroupBinding> list_token_channel_group_bindings(long long token_id);
    bool replace_token_channel_groups(long long token_id, const std::vector<std::string> &names);
    std::vector<TokenChannelGroupBinding> list_effective_token_channel_group_bindings(long long token_id);
    std::vector<std::string> list_effective_token_channel_groups(long long token_id);

    std::vector<TokenModelMapping> list_token_model_mappings(long long token_id);
    std::vector<TokenModelTargetOption> list_token_model_mapping_targets(long long token_id);
    bool replace_token_model_mappings(long long token_id, const std::vector<TokenModelMappingCreate> &mappings);
    bool prune_token_model_mappings(long long token_id);

private:
    MysqlConnection &conn_;
};

} // namespace revlm
