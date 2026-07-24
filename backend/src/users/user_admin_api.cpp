#include "users/user_admin_api.hpp"

#include "auth/security.hpp"
#include "users/user_api.hpp"
#include "users/users.hpp"
#include "store/database.hpp"
#include "util/json_convert.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"
#include "util/user_input.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace revlm
{
namespace
{

json admin_users_json(std::vector<User> users)
{
    std::sort(users.begin(), users.end(), [](const User &a, const User &b) { return a.id > b.id; });
    json data = json::array();
    for (const User &user : users) {
        data.push_back(to_json(user));
    }
    return data;
}

} // namespace

json admin_list_users_response(std::string_view raw_request, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_admin(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    try {
        UserStore &store = UserStore::instance();
        return json({ { "success", true }, { "data", admin_users_json(store.list_users()) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "查询失败" } });
    }
}

json admin_create_user_response(std::string_view raw_request, std::string_view body, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_admin(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
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
        const std::string role = normalize_user_role(json_object_string(*object, "role"), "user");
        const std::string password_hash = hash_password(password);
        UserStore &store = UserStore::instance();
        if (store.get_user_by_username(username).id != 0) {
            return json({ { "success", false }, { "message", "账号名已被占用" } });
        }
        User user(email, username, password_hash, role);
        user.status = 1;
        const long long user_id = store.create_user(std::move(user));
        return json({ { "success", true }, { "data", json{ { "id", user_id } } } });
    } catch (const std::invalid_argument &err) {
        return json({ { "success", false }, { "message", err.what() } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "创建失败（可能邮箱或账号名已存在）" } });
    }
}

json admin_update_user_response(long long user_id, std::string_view raw_request, std::string_view body,
                                std::string *set_cookie)
{
    json error;
    const auto actor = api_authenticated_admin(raw_request, error, set_cookie);
    if (!actor.has_value()) {
        return error;
    }
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return json({ { "success", false }, { "message", "无效的参数" } });
    }
    try {
        UserStore &store = UserStore::instance();
        User target = store.get_user_by_id(user_id);
        if (target.id == 0) {
            return json({ { "success", false }, { "message", "not found" } });
        }
        if (user_id == actor->id) {
            if (object->contains("status")) {
                int status = 0;
                (void)parse_json_int((*object)["status"], status);
                if (status == 0) {
                    return json({ { "success", false }, { "message", "不能禁用当前登录用户" } });
                }
            }
            if (object->contains("role")) {
                const std::string role = trim_ascii(json_object_string(*object, "role"));
                if (!role.empty() && role != "root") {
                    return json({ { "success", false }, { "message", "不能修改当前登录用户的 root 角色" } });
                }
            }
        }
        if (object->contains("email")) {
            target.email = normalize_email(json_object_string(*object, "email"));
        }
        if (object->contains("status")) {
            int status = 0;
            if (parse_json_int((*object)["status"], status)) {
                target.status = normalize_user_status(status);
            }
        }
        if (object->contains("role")) {
            target.role = normalize_user_role(json_object_string(*object, "role"), target.role);
        }
        if (!store.update_user(target)) {
            return json({ { "success", false }, { "message", "保存失败" } });
        }

        return json({ { "success", true } });
    } catch (const std::invalid_argument &err) {
        return json({ { "success", false }, { "message", err.what() } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "保存失败" } });
    }
}

json admin_reset_user_password_response(long long user_id, std::string_view raw_request, std::string_view body,
                                        std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_admin(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return json({ { "success", false }, { "message", "无效的参数" } });
    }
    try {
        const std::string password = json_object_string(*object, "password");
        if (trim_ascii(password).empty()) {
            return json({ { "success", false }, { "message", "新密码不能为空" } });
        }
        UserStore &store = UserStore::instance();
        User target = store.get_user_by_id(user_id);
        if (target.id == 0) {
            return json({ { "success", false }, { "message", "用户不存在" } });
        }
        target.password_hash = hash_password(password);
        if (!store.update_user(target)) {
            return json({ { "success", false }, { "message", "保存失败" } });
        }
        return json({ { "success", true } });
    } catch (const std::invalid_argument &err) {
        return json({ { "success", false }, { "message", err.what() } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "保存失败" } });
    }
}

json admin_add_user_balance_response(long long user_id, std::string_view raw_request, std::string_view body,
                                     std::string *set_cookie)
{
    json error;
    const auto actor = api_authenticated_admin(raw_request, error, set_cookie);
    if (!actor.has_value()) {
        return error;
    }
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return json({ { "success", false }, { "message", "无效的参数" } });
    }
    try {
        UserStore &store = UserStore::instance();
        User target = store.get_user_by_id(user_id);
        if (target.id == 0) {
            return json({ { "success", false }, { "message", "用户不存在" } });
        }
        const std::string amount_raw = normalize_usd_amount(json_object_string(*object, "amount_usd"));
        target.balance_usd += std::stod(amount_raw);
        if (!store.update_user(target)) {
            return json({ { "success", false }, { "message", "用户不存在" } });
        }
        const double balance = UserStore::instance().get_user_balance_usd(user_id);
        return json({ { "success", true }, { "data", json{ { "balance_usd", balance } } } });
    } catch (const std::invalid_argument &err) {
        return json({ { "success", false }, { "message", err.what() } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", std::string{ "入账失败：" } + err.what() } });
    }
}

json admin_delete_user_response(long long user_id, std::string_view raw_request, std::string *set_cookie)
{
    json error;
    const auto actor = api_authenticated_admin(raw_request, error, set_cookie);
    if (!actor.has_value()) {
        return error;
    }
    if (user_id == actor->id) {
        return json({ { "success", false }, { "message", "不能删除当前登录用户" } });
    }
    try {
        UserStore &store = UserStore::instance();
        if (!store.delete_user(user_id)) {
            return json({ { "success", false }, { "message", "用户不存在" } });
        }
        return json({ { "success", true } });
    } catch (const std::invalid_argument &err) {
        return json({ { "success", false }, { "message", err.what() } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "删除失败" } });
    }
}

} // namespace revlm
