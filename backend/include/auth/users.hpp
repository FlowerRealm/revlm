#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "server/tokens.hpp"
#include "store/database.hpp"

namespace revlm
{

#pragma db object table("users")
class User {
public:
    User() = default;
    User(std::string email, std::string username, std::string password_hash, std::string role)
        : email(std::move(email))
        , username(std::move(username))
        , password_hash(std::move(password_hash))
        , role(std::move(role))
    {
    }
    User(long long id, std::string email, std::string username, std::string password_hash, std::string role, int status)
        : id(id)
        , email(std::move(email))
        , username(std::move(username))
        , password_hash(std::move(password_hash))
        , role(std::move(role))
        , status(status)
    {
    }

#pragma db id auto
    long long id = 0;
    std::string email;
    std::string username;
    std::string password_hash;
    std::string role;
    int status = 0;
#pragma db transient
    std::string balance_usd;
};

class UserStore {
public:
    explicit UserStore(odb::database &db);

    TokenStore &tokens();

    long long count_users();
    long long create_user(User user);
    User get_user_by_id(long long id);
    User get_user_by_email(std::string_view email);
    User get_user_by_username(std::string_view username);
    std::vector<User> list_users();
    bool update_user(const User &user);
    bool delete_user(long long user_id);

private:
    odb::database &db_;
    TokenStore tokens_;
};

} // namespace revlm
