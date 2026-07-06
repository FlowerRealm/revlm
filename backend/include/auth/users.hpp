#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "store/mysql.hpp"

namespace revlm
{

struct User {
    long long id = 0;
    std::string email;
    std::string username;
    std::string password_hash;
    std::string role;
    int status = 0;
    std::string created_at;
};

struct CreateUserInput {
    std::string email;
    std::string username;
    std::string password_hash;
    std::string role;
};

struct AdminUserView {
    long long id = 0;
    std::string email;
    std::string username;
    std::string role;
    int status = 0;
    std::string balance_usd;
    std::string created_at;
};

struct SessionBinding {
    long long user_id = 0;
    std::string route_key_hash;
    std::string payload_json;
    std::string expires_at;
};

struct SessionCookie {
    long long user_id = 0;
    long long expires_unix = 0;
    std::string key;
    std::string value;
};

std::string hash_password(std::string_view password);
bool check_password(std::string_view hash, std::string_view password);

SessionCookie make_session_cookie(long long user_id, std::string_view secret);
std::optional<SessionCookie> verify_session_cookie_value(std::string_view cookie_value, std::string_view secret);
std::string session_binding_hash(std::string_view session_key);
std::string session_secret_for_config(const Config &config);

std::optional<std::string> cookie_value(std::string_view raw_request, std::string_view name);
std::string set_session_cookie_header(std::string_view value, std::string_view raw_request);
std::string clear_session_cookie_header(std::string_view raw_request);

struct WebSessionAuth {
    bool ok = false;
    bool clear_cookie = false;
    std::string failure_message;
    std::optional<User> user;
    std::string session_binding_hash;
};

WebSessionAuth authenticate_web_session(std::string_view raw_request, const Config &config,
                                        bool capture_binding_hash = false);
WebSessionAuth authenticate_root_web_session(std::string_view raw_request, const Config &config);

class UserStore {
public:
    explicit UserStore(MysqlConnection &conn);

    long long count_users();
    long long create_user(const CreateUserInput &input);
    std::optional<User> get_user_by_id(long long id);
    std::optional<User> get_user_by_id_for_update(long long id);
    std::optional<User> get_user_by_email(std::string_view email);
    std::optional<User> get_user_by_username(std::string_view username);
    std::vector<AdminUserView> list_admin_users();
    void update_user_email(long long user_id, std::string_view email);
    void update_user_password_hash(long long user_id, std::string_view password_hash);
    bool reset_user_password_hash(long long user_id, std::string_view password_hash);
    void set_user_role(long long user_id, std::string_view role);
    void set_user_status(long long user_id, int status);
    std::optional<std::string> add_user_balance_usd(long long user_id, std::string_view amount_usd);
    bool delete_user(long long user_id);

    std::optional<SessionBinding> get_session_binding_payload(long long user_id, std::string_view route_key_hash);
    void upsert_session_binding_payload(long long user_id, std::string_view route_key_hash,
                                        std::string_view payload_json, std::string_view expires_at);
    void delete_session_binding(long long user_id, std::string_view route_key_hash);
    void delete_all_session_bindings(long long user_id);

private:
    std::optional<User> get_user_by_sql(const std::string &sql);

    MysqlConnection &conn_;
};

} // namespace revlm
