#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "auth/users.hpp"
#include "config/config.hpp"
#include "store/mysql.hpp"

namespace revlm
{

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

class SessionStore {
public:
    explicit SessionStore(MysqlConnection &conn);

    std::optional<SessionBinding> get_session_binding_payload(long long user_id, std::string_view route_key_hash);
    void upsert_session_binding_payload(long long user_id, std::string_view route_key_hash,
                                        std::string_view payload_json, std::string_view expires_at);
    void delete_session_binding(long long user_id, std::string_view route_key_hash);
    void delete_all_session_bindings(long long user_id);

private:
    MysqlConnection &conn_;
};

} // namespace revlm
