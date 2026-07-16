#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <odb/nullable.hxx>

#include "request/request.hpp"

namespace revlm
{

#pragma db object table("user_tokens")
struct UserToken {
#pragma db id auto
    long long id = 0;
    long long user_id = 0;
    odb::nullable<std::string> name;
    std::string token_hash;
    odb::nullable<std::string> token_plain;
    int status = 0;
    long long channel_id = 0;
};

struct TokenAuth {
    long long user_id = 0;
    long long token_id = 0;
    std::string role;
    long long channel_id = 0;
};

std::string new_random_token(std::string_view prefix = "sk_", int bytes_len = 32);
std::string token_hash(std::string_view raw_token);
std::string hex_encode(std::string_view bytes);

class TokenStore {
public:
    explicit TokenStore(odb::database &db);

    RequestStore &requests();

    long long create_user_token(long long user_id, const odb::nullable<std::string> &name, std::string_view raw_token);
    std::vector<UserToken> list_user_tokens(long long user_id);
    std::optional<UserToken> get_user_token_by_id(long long user_id, long long token_id);
    std::optional<std::string> reveal_user_token(long long user_id, long long token_id);
    bool rotate_user_token(long long user_id, long long token_id, std::string_view raw_token);
    void revoke_user_token(long long user_id, long long token_id);
    bool delete_user_token(long long user_id, long long token_id);
    std::optional<TokenAuth> get_token_auth_by_raw_token(std::string_view raw_token);
    bool set_token_channel(long long user_id, long long token_id, long long channel_id);

private:
    odb::database &db_;
    RequestStore requests_;
};

} // namespace revlm
