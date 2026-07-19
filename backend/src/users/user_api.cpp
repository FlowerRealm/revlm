#include "users/user_api.hpp"

#include "auth/session.hpp"
#include "util/json.hpp"
#include "util/json_convert.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"
#include "util/user_input.hpp"

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace revlm
{

json web_session_auth_failure_response(const WebSessionAuth &auth, std::string_view raw_request,
                                       std::string *set_cookie)
{
    if (auth.clear_cookie && set_cookie != nullptr) {
        *set_cookie = clear_session_cookie_header(raw_request);
    }
    const std::string message = auth.failure_message.empty() ? "未登录" : auth.failure_message;
    return json({ { "success", false }, { "message", message } });
}

json register_response(std::string_view raw_request, std::string_view body, std::string *set_cookie)
{
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return json({ { "success", false }, { "message", "无效的参数" } });
    }

    try {
        const std::string email = normalize_email(json_object_string(*object, "email"));
        const std::string username = normalize_username(json_object_string(*object, "username"));
        const std::string password = json_object_string(*object, "password");
        if (password.empty()) {
            return json({ { "success", false }, { "message", "邮箱或密码不能为空" } });
        }
        const std::string password_hash = hash_password(password);

        UserStore &store = UserStore::instance();
        SessionStore &sessions = SessionStore::instance();
        const std::string role = store.count_users() == 0 ? "root" : "user";
        User user(email, username, password_hash, role);
        user.status = 1;
        user.id = store.create_user(user);
        const SessionCookie session = sessions.create(user.id);
        if (set_cookie != nullptr) {
            *set_cookie = set_session_cookie_header(session.value, raw_request);
        }
        return json({ { "success", true }, { "data", to_json(user) } });
    } catch (const std::invalid_argument &err) {
        return json({ { "success", false }, { "message", err.what() } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "创建用户失败（可能邮箱或账号名已存在）" } });
    }
}

json login_response(std::string_view raw_request, std::string_view body, std::string *set_cookie)
{
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return json({ { "success", false }, { "message", "无效的参数" } });
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
        return json({ { "success", false }, { "message", "无效的参数" } });
    }
    try {
        UserStore &store = UserStore::instance();
        SessionStore &sessions = SessionStore::instance();
        User user = store.get_user_by_email(lowercase_ascii(login));
        if (user.id == 0) {
            user = store.get_user_by_username(login);
        }
        if (user.id == 0 || user.status != 1 || !check_password(user.password_hash, password)) {
            return json({ { "success", false }, { "message", "邮箱/账号名或密码错误" } });
        }
        const SessionCookie session = sessions.create(user.id);
        if (set_cookie != nullptr) {
            *set_cookie = set_session_cookie_header(session.value, raw_request);
        }
        return json({ { "success", true }, { "data", to_json(user) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "邮箱/账号名或密码错误" } });
    }
}

json self_response(std::string_view raw_request, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    return json({ { "success", true }, { "data", to_json(*user) } });
}

json logout_response(std::string_view raw_request, std::string *set_cookie)
{
    const WebSessionAuth auth = authenticate_web_session(raw_request);
    if (!auth.ok) {
        return web_session_auth_failure_response(auth, raw_request, set_cookie);
    }
    try {
        SessionStore::instance().delete_by_token_hash(auth.token_hash);
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "无法清理会话，请重试" } });
    }
    if (set_cookie != nullptr) {
        *set_cookie = clear_session_cookie_header(raw_request);
    }
    return json({ { "success", true } });
}

std::optional<User> api_authenticated_user(std::string_view raw_request, json &error, std::string *set_cookie)
{
    const WebSessionAuth auth = authenticate_web_session(raw_request);
    if (auth.ok) {
        return auth.user;
    }
    error = web_session_auth_failure_response(auth, raw_request, set_cookie);
    return std::nullopt;
}

std::optional<User> api_authenticated_admin(std::string_view raw_request, json &error, std::string *set_cookie)
{
    const WebSessionAuth auth = authenticate_root_web_session(raw_request);
    if (auth.ok) {
        return auth.user;
    }
    error = web_session_auth_failure_response(auth, raw_request, set_cookie);
    return std::nullopt;
}

json account_email_response(std::string_view raw_request, std::string_view body, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }

    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return json({ { "success", false }, { "message", "无效的参数" } });
    }

    const std::string current_password = json_object_string(*object, "current_password");
    if (current_password.empty()) {
        return json({ { "success", false }, { "message", "无效的参数" } });
    }

    try {
        const std::string email = normalize_email(json_object_string(*object, "email"));
        UserStore &store = UserStore::instance();
        SessionStore &sessions = SessionStore::instance();
        User locked_user = store.get_user_by_id(user->id);
        if (locked_user.id == 0) {
            if (set_cookie != nullptr) {
                *set_cookie = clear_session_cookie_header(raw_request);
            }
            return json({ { "success", false }, { "message", "未登录" } });
        }
        if (locked_user.status != 1) {
            if (set_cookie != nullptr) {
                *set_cookie = clear_session_cookie_header(raw_request);
            }
            return json({ { "success", false }, { "message", "账号已被禁用" } });
        }
        if (!check_password(locked_user.password_hash, current_password)) {
            return json({ { "success", false }, { "message", "旧密码错误" } });
        }
        locked_user.email = email;
        if (!store.update_user(locked_user)) {
            return json({ { "success", false }, { "message", "更新邮箱失败（可能邮箱已存在）" } });
        }
        sessions.delete_all_for_user(user->id);
        if (set_cookie != nullptr) {
            *set_cookie = clear_session_cookie_header(raw_request);
        }
        return json({ { "success", true }, { "data", json{ { "force_logout", true } } } });
    } catch (const std::invalid_argument &err) {
        return json({ { "success", false }, { "message", err.what() } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "更新邮箱失败（可能邮箱已存在）" } });
    }
}

json account_password_response(std::string_view raw_request, std::string_view body, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }

    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return json({ { "success", false }, { "message", "无效的参数" } });
    }

    const std::string old_password = json_object_string(*object, "old_password");
    const std::string new_password = json_object_string(*object, "new_password");
    if (old_password.empty() || new_password.empty()) {
        return json({ { "success", false }, { "message", "无效的参数" } });
    }

    try {
        const std::string password_hash = hash_password(new_password);
        UserStore &store = UserStore::instance();
        SessionStore &sessions = SessionStore::instance();
        User locked_user = store.get_user_by_id(user->id);
        if (locked_user.id == 0) {
            if (set_cookie != nullptr) {
                *set_cookie = clear_session_cookie_header(raw_request);
            }
            return json({ { "success", false }, { "message", "未登录" } });
        }
        if (locked_user.status != 1) {
            if (set_cookie != nullptr) {
                *set_cookie = clear_session_cookie_header(raw_request);
            }
            return json({ { "success", false }, { "message", "账号已被禁用" } });
        }
        if (!check_password(locked_user.password_hash, old_password)) {
            return json({ { "success", false }, { "message", "旧密码错误" } });
        }
        locked_user.password_hash = password_hash;
        if (!store.update_user(locked_user)) {
            return json({ { "success", false }, { "message", "更新密码失败" } });
        }
        sessions.delete_all_for_user(user->id);
        if (set_cookie != nullptr) {
            *set_cookie = clear_session_cookie_header(raw_request);
        }
        return json({ { "success", true }, { "data", json{ { "force_logout", true } } } });
    } catch (const std::invalid_argument &err) {
        return json({ { "success", false }, { "message", err.what() } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "更新密码失败" } });
    }
}

} // namespace revlm
