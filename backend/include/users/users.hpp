#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "users/tokens.hpp"

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
    double balance_usd = 0;
};

class UserStore {
public:
    static UserStore &instance();

    TokenStore &tokens();

    long long count_users();
    long long create_user(User user);
    User get_user_by_id(long long id);
    User get_user_by_email(std::string_view email);
    User get_user_by_username(std::string_view username);
    std::vector<User> list_users();
    bool update_user(const User &user);
    bool delete_user(long long user_id);

    double get_user_balance_usd(long long user_id);
    bool has_positive_user_balance(long long user_id);
    bool debit_user_balance_usd(long long user_id, double delta_usd, double *remaining_usd = nullptr);

    UserStore(const UserStore &) = delete;
    UserStore &operator=(const UserStore &) = delete;

private:
    friend void reset_stores_for_test();
    UserStore();
    static void reset_instance();

    odb::database &db_;
    TokenStore tokens_;
};

} // namespace revlm
