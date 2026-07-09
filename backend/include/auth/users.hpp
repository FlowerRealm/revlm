#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "store/mysql.hpp"

namespace revlm
{

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
    User(long long id, std::string email, std::string username, std::string password_hash, std::string role, int status,
         std::string created_at)
        : id(id)
        , email(std::move(email))
        , username(std::move(username))
        , password_hash(std::move(password_hash))
        , role(std::move(role))
        , status(status)
        , created_at(std::move(created_at))
    {
    }

    long long id = 0;
    std::string email;
    std::string username;
    std::string password_hash;
    std::string role;
    int status = 0;
    std::string balance_usd;
    std::string created_at;
};

std::string hash_password(std::string_view password);
bool check_password(std::string_view hash, std::string_view password);

class UserStore {
public:
    explicit UserStore(MysqlConnection &conn);

    long long count_users();
    long long create_user(const User &user);
    std::optional<User> get_user_by_id(long long id);
    std::optional<User> get_user_by_id_for_update(long long id);
    std::optional<User> get_user_by_email(std::string_view email);
    std::optional<User> get_user_by_username(std::string_view username);
    std::vector<User> list_admin_users();
    void update_user_email(long long user_id, std::string_view email);
    void update_user_password_hash(long long user_id, std::string_view password_hash);
    void set_user_role(long long user_id, std::string_view role);
    void set_user_status(long long user_id, int status);
    std::optional<std::string> add_user_balance_usd(long long user_id, std::string_view amount_usd);
    bool delete_user(long long user_id);

private:
    std::optional<User> get_user_by_sql(const std::string &sql);

    MysqlConnection &conn_;
};

} // namespace revlm
