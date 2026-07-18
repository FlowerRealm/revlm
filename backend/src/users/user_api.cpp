#include "users/user_api.hpp"

#include "auth/session.hpp"
#include "util/datetime.hpp"
#include "util/json.hpp"
#include "util/json_convert.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"
#include "util/user_input.hpp"

#include <ctime>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace revlm
{
namespace
{

std::optional<User> authenticated_user(std::string_view raw_request, std::string &failure_message, bool &clear_cookie,
                                       std::string *binding_hash = nullptr)
{
    const WebSessionAuth auth = authenticate_web_session(raw_request, binding_hash != nullptr);
    failure_message = auth.failure_message;
    clear_cookie = auth.clear_cookie;
    if (!auth.ok) {
        return std::nullopt;
    }
    if (binding_hash != nullptr) {
        *binding_hash = auth.session_binding_hash;
    }
    return auth.user;
}

} // namespace

HttpResponse register_response(std::string_view raw_request, std::string_view body, std::string_view request_id)
{
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    try {
        const std::string email = normalize_email(json_object_string(*object, "email"));
        const std::string username = normalize_username(json_object_string(*object, "username"));
        const std::string password = json_object_string(*object, "password");
        if (password.empty()) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "邮箱或密码不能为空" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        const std::string password_hash = hash_password(password);

        UserStore &store = UserStore::instance();
        SessionStore &sessions = SessionStore::instance();
        const std::string role = store.count_users() == 0 ? "root" : "user";
        User user(email, username, password_hash, role);
        user.status = 1;
        user.id = store.create_user(user);
        const SessionCookie session = make_session_cookie(user.id, session_secret());
        sessions.upsert_session_binding_payload(
            user.id, session_binding_hash(session.key), "web",
            to_mysql_datetime(unix_to_sys(static_cast<std::time_t>(session.expires_unix))));
        return http_response(200, "OK", json({ { "success", true }, { "data", to_json(user) } }),
                             { { "X-Request-Id", std::string{ request_id } },
                               Header{ "Set-Cookie", set_session_cookie_header(session.value, raw_request) } });
    } catch (const std::invalid_argument &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return http_response(200, "OK",
                             json({ { "success", false }, { "message", "创建用户失败（可能邮箱或账号名已存在）" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse login_response(std::string_view raw_request, std::string_view request_id, std::string_view body)
{
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
    std::string login = trim_ascii(json_object_string(*object, "login"));
    if (login.empty()) {
        login = trim_ascii(json_object_string(*object, "username"));
    }
    if (login.empty()) {
        login = trim_ascii(json_object_string(*object, "email"));
    }
    const std::string password = json_object_string(*object, "password");
    if (login.empty() || password.empty()) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
    try {
        UserStore &store = UserStore::instance();
        SessionStore &sessions = SessionStore::instance();
        User user = store.get_user_by_email(lowercase_ascii(login));
        if (user.id == 0) {
            user = store.get_user_by_username(login);
        }
        if (user.id == 0 || user.status != 1 || !check_password(user.password_hash, password)) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "邮箱/账号名或密码错误" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        const SessionCookie session = make_session_cookie(user.id, session_secret());
        sessions.upsert_session_binding_payload(
            user.id, session_binding_hash(session.key), "web",
            to_mysql_datetime(unix_to_sys(static_cast<std::time_t>(session.expires_unix))));
        return http_response(200, "OK", json({ { "success", true }, { "data", to_json(user) } }),
                             { { "X-Request-Id", std::string{ request_id } },
                               Header{ "Set-Cookie", set_session_cookie_header(session.value, raw_request) } });
    } catch (const std::exception &) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "邮箱/账号名或密码错误" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse self_response(std::string_view raw_request, std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_user(raw_request, failure, clear_cookie);
    if (!user.has_value()) {
        std::vector<Header> headers;
        headers.push_back({ "X-Request-Id", std::string{ request_id } });
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return http_response(200, "OK", json({ { "success", false }, { "message", failure } }), headers);
    }
    return http_response(200, "OK", json({ { "success", true }, { "data", to_json(*user) } }),
                         { { "X-Request-Id", std::string{ request_id } } });
}

HttpResponse logout_response(std::string_view raw_request, std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    std::string binding_hash;
    const auto user = authenticated_user(raw_request, failure, clear_cookie, &binding_hash);
    if (!user.has_value()) {
        std::vector<Header> headers;
        headers.push_back({ "X-Request-Id", std::string{ request_id } });
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return http_response(200, "OK", json({ { "success", false }, { "message", failure } }), headers);
    }
    try {
        SessionStore &sessions = SessionStore::instance();
        sessions.delete_session_binding(user->id, binding_hash);
    } catch (const std::exception &) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "无法清理会话，请重试" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
    return http_response(200, "OK", json({ { "success", true } }),
                         { { "X-Request-Id", std::string{ request_id } },
                           Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
}

std::optional<User> api_authenticated_user(std::string_view raw_request, std::string_view request_id,
                                           HttpResponse &response)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_user(raw_request, failure, clear_cookie);
    if (user.has_value()) {
        return user;
    }
    std::vector<Header> headers;
    headers.push_back({ "X-Request-Id", std::string{ request_id } });
    if (clear_cookie) {
        headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
    }
    response = http_response(200, "OK", json({ { "success", false }, { "message", failure } }), headers);
    return std::nullopt;
}

HttpResponse account_email_response(std::string_view raw_request, std::string_view body, std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_user(raw_request, failure, clear_cookie);
    if (!user.has_value()) {
        std::vector<Header> headers;
        headers.push_back({ "X-Request-Id", std::string{ request_id } });
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return http_response(200, "OK", json({ { "success", false }, { "message", failure } }), headers);
    }

    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    const std::string current_password = json_object_string(*object, "current_password");
    if (current_password.empty()) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    try {
        const std::string email = normalize_email(json_object_string(*object, "email"));
        UserStore &store = UserStore::instance();
        SessionStore &sessions = SessionStore::instance();
        User locked_user = store.get_user_by_id(user->id);
        if (locked_user.id == 0) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "未登录" } }),
                                 { { "X-Request-Id", std::string{ request_id } },
                                   Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
        }
        if (locked_user.status != 1) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "账号已被禁用" } }),
                                 { { "X-Request-Id", std::string{ request_id } },
                                   Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
        }
        if (!check_password(locked_user.password_hash, current_password)) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "旧密码错误" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        locked_user.email = email;
        if (!store.update_user(locked_user)) {
            return http_response(200, "OK",
                                 json({ { "success", false }, { "message", "更新邮箱失败（可能邮箱已存在）" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        sessions.delete_all_session_bindings(user->id);
        return http_response(200, "OK", json({ { "success", true }, { "data", json{ { "force_logout", true } } } }),
                             { { "X-Request-Id", std::string{ request_id } },
                               Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
    } catch (const std::invalid_argument &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "更新邮箱失败（可能邮箱已存在）" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse account_password_response(std::string_view raw_request, std::string_view body, std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_user(raw_request, failure, clear_cookie);
    if (!user.has_value()) {
        std::vector<Header> headers;
        headers.push_back({ "X-Request-Id", std::string{ request_id } });
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return http_response(200, "OK", json({ { "success", false }, { "message", failure } }), headers);
    }

    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    const std::string old_password = json_object_string(*object, "old_password");
    const std::string new_password = json_object_string(*object, "new_password");
    if (old_password.empty() || new_password.empty()) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    try {
        const std::string password_hash = hash_password(new_password);
        UserStore &store = UserStore::instance();
        SessionStore &sessions = SessionStore::instance();
        User locked_user = store.get_user_by_id(user->id);
        if (locked_user.id == 0) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "未登录" } }),
                                 { { "X-Request-Id", std::string{ request_id } },
                                   Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
        }
        if (locked_user.status != 1) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "账号已被禁用" } }),
                                 { { "X-Request-Id", std::string{ request_id } },
                                   Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
        }
        if (!check_password(locked_user.password_hash, old_password)) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "旧密码错误" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        locked_user.password_hash = password_hash;
        if (!store.update_user(locked_user)) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "更新密码失败" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        sessions.delete_all_session_bindings(user->id);
        return http_response(200, "OK", json({ { "success", true }, { "data", json{ { "force_logout", true } } } }),
                             { { "X-Request-Id", std::string{ request_id } },
                               Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
    } catch (const std::invalid_argument &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "更新密码失败" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
}

} // namespace revlm
