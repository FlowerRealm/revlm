#include "users/token_api.hpp"

#include "channels/channel_groups.hpp"
#include "users/tokens.hpp"
#include "users/user_api.hpp"
#include "util/json_convert.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace revlm
{

std::optional<std::string> extract_api_token(const ::httplib::Request &req)
{
    const std::string authorization = trim_ascii(req.get_header_value("Authorization"));
    const size_t sep = authorization.find(' ');
    if (sep != std::string::npos) {
        const std::string scheme = lowercase_ascii(trim_ascii(authorization.substr(0, sep)));
        const std::string token = trim_ascii(authorization.substr(sep + 1));
        if (scheme == "bearer" && !token.empty())
            return token;
    }

    const std::string api_key = trim_ascii(req.get_header_value("x-api-key"));
    if (!api_key.empty())
        return api_key;

    return std::nullopt;
}

std::optional<long long> authenticate_api_token(const ::httplib::Request &req, long long &user_id, long long &token_id)
{
    const auto raw_token = extract_api_token(req);
    if (!raw_token.has_value()) {
        return std::nullopt;
    }
    try {
        return UserStore::instance().tokens().resolve_token_channel_group_by_raw_token(*raw_token, user_id, token_id);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

json list_user_tokens_response(const User &user)
{
    try {
        UserStore &users = UserStore::instance();
        TokenStore &store = users.tokens();
        const std::vector<UserToken> tokens = store.list_user_tokens(user.id);
        json data = json::array();
        for (const UserToken &token : tokens) {
            data.push_back(to_json(token));
        }
        return json({ { "success", true }, { "data", std::move(data) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "查询 Token 列表失败" } });
    }
}

json create_user_token_response(std::string_view raw_request, std::string_view body, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }

    std::optional<std::string> token_name;
    if (!trim_ascii(body).empty()) {
        const auto object = parse_json_object(body);
        if (!object.has_value()) {
            return json({ { "success", false }, { "message", "无效的参数" } });
        }
        std::string name = trim_ascii(json_object_string(*object, "name"));
        if (!name.empty()) {
            token_name = name;
        }
    }

    try {
        const std::string raw_token = new_random_token("sk_", 32);
        UserStore &users = UserStore::instance();
        TokenStore &store = users.tokens();
        odb::nullable<std::string> name;
        if (token_name.has_value()) {
            name = *token_name;
        }
        const long long token_id = store.create_user_token(user->id, name, raw_token);
        return json({ { "success", true }, { "data", json({ { "token_id", token_id }, { "token", raw_token } }) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "创建令牌失败" } });
    }
}

json reveal_user_token_response(std::string_view raw_request, long long token_id, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    if (token_id <= 0) {
        return json({ { "success", false }, { "message", "token_id 不合法" } });
    }
    try {
        UserStore &users = UserStore::instance();
        TokenStore &store = users.tokens();
        const auto token = store.reveal_user_token(user->id, token_id);
        if (!token.has_value()) {
            return json({ { "success", false }, { "message", "令牌不存在" } });
        }
        return json({ { "success", true }, { "data", json({ { "token_id", token_id }, { "token", *token } }) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "查看失败" } });
    }
}

json rotate_user_token_response(std::string_view raw_request, long long token_id, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    if (token_id <= 0) {
        return json({ { "success", false }, { "message", "token_id 不合法" } });
    }
    try {
        const std::string raw_token = new_random_token("sk_", 32);
        UserStore &users = UserStore::instance();
        TokenStore &store = users.tokens();
        if (!store.rotate_user_token(user->id, token_id, raw_token)) {
            return json({ { "success", false }, { "message", "令牌不存在" } });
        }
        return json({ { "success", true }, { "data", json({ { "token_id", token_id }, { "token", raw_token } }) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "重新生成失败" } });
    }
}

json revoke_user_token_response(std::string_view raw_request, long long token_id, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    if (token_id <= 0) {
        return json({ { "success", false }, { "message", "token_id 不合法" } });
    }
    try {
        UserStore &users = UserStore::instance();
        TokenStore &store = users.tokens();
        store.revoke_user_token(user->id, token_id);
        return json({ { "success", true } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "撤销失败" } });
    }
}

json delete_user_token_response(std::string_view raw_request, long long token_id, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    if (token_id <= 0) {
        return json({ { "success", false }, { "message", "token_id 不合法" } });
    }
    try {
        UserStore &users = UserStore::instance();
        TokenStore &store = users.tokens();
        if (!store.delete_user_token(user->id, token_id)) {
            return json({ { "success", false }, { "message", "令牌不存在" } });
        }
        return json({ { "success", true } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "删除失败" } });
    }
}

json token_channel_response(std::string_view raw_request, long long token_id, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    if (token_id <= 0) {
        return json({ { "success", false }, { "message", "token_id 不合法" } });
    }
    try {
        UserStore &users = UserStore::instance();
        TokenStore &store = users.tokens();
        const auto token = store.get_user_token_by_id(user->id, token_id);
        if (!token.has_value()) {
            return json({ { "success", false }, { "message", "令牌不存在" } });
        }

        ChannelGroupStore &group_store = ChannelGroupStore::instance();
        json allowed_json = json::array();
        for (const ChannelGroup &group : group_store.list_channel_groups()) {
            if (!group.status && group.id != token->channel_group_id) {
                continue;
            }
            json item;
            item["id"] = group.id;
            item["name"] = group.name;
            item["description"] = group.description;
            item["status"] = group.status;
            item["price_multiplier"] = group.price_multiplier;
            allowed_json.push_back(std::move(item));
        }

        json data;
        data["token_id"] = token_id;
        data["channel_group_id"] = token->channel_group_id;
        data["allowed_channel_groups"] = std::move(allowed_json);
        return json({ { "success", true }, { "data", std::move(data) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "查询 Token 渠道组失败" } });
    }
}

json set_token_channel_response(std::string_view raw_request, long long token_id, std::string_view body,
                                std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    if (token_id <= 0) {
        return json({ { "success", false }, { "message", "token_id 不合法" } });
    }

    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return json({ { "success", false }, { "message", "无效的参数" } });
    }
    if (!object->contains("channel_group_id")) {
        return json({ { "success", false }, { "message", "无效的参数" } });
    }
    const json group_field = (*object)["channel_group_id"];
    if (!group_field.is_number()) {
        return json({ { "success", false }, { "message", "无效的参数" } });
    }
    const long long channel_group_id = group_field.as_int64().value_or(0);
    if (channel_group_id <= 0) {
        return json({ { "success", false }, { "message", "无效的参数" } });
    }

    try {
        UserStore &users = UserStore::instance();
        TokenStore &store = users.tokens();
        if (!store.set_token_channel_group(user->id, token_id, channel_group_id)) {
            return json({ { "success", false }, { "message", "令牌不存在" } });
        }
        return json({ { "success", true } });
    } catch (const std::invalid_argument &err) {
        return json({ { "success", false }, { "message", err.what() } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "设置 Token 渠道组失败" } });
    }
}

} // namespace revlm
