#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <odb/database.hxx>

#include "users/users.hpp"

namespace revlm
{

#pragma db object table("sessions")
struct Session {
#pragma db id
    std::string token_hash;
    long long user_id = 0;
    std::string expires_at;
};

struct SessionCookie {
    std::string value;
    std::string token_hash;
    long long expires_unix = 0;
};

SessionCookie make_session_cookie();
std::string session_token_hash(std::string_view opaque_token);

std::optional<std::string> cookie_value(std::string_view raw_request, std::string_view name);
std::string set_session_cookie_header(std::string_view value, std::string_view raw_request);
std::string clear_session_cookie_header(std::string_view raw_request);

struct WebSessionAuth {
    bool ok = false;
    bool clear_cookie = false;
    std::string failure_message;
    std::optional<User> user;
    std::string token_hash;
};

WebSessionAuth authenticate_web_session(std::string_view raw_request);
WebSessionAuth authenticate_root_web_session(std::string_view raw_request);

class SessionStore {
public:
    static SessionStore &instance();

    SessionCookie create(long long user_id);
    std::optional<Session> get_by_token_hash(std::string_view token_hash);
    void delete_by_token_hash(std::string_view token_hash);
    void delete_all_for_user(long long user_id);

    SessionStore(const SessionStore &) = delete;
    SessionStore &operator=(const SessionStore &) = delete;

private:
    friend void reset_stores_for_test();
    SessionStore();
    static void reset_instance();

    odb::database &db_;
};

} // namespace revlm
