#include "server/http_dispatch.hpp"
#include "server/http_server.hpp"
#include "auth/security.hpp"
#include "users/users.hpp"
#include "users/user_api.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "config/config.hpp"
#include "models/models.hpp"
#include "proxy/openai_chat.hpp"
#include "proxy/anthropics_messages.hpp"
#include "proxy/openai_responses.hpp"
#include "proxy/gateway.hpp"
#include "request/request.hpp"
#include "users/tokens.hpp"
#include "store/database.hpp"
#include "util/datetime.hpp"
#include "request/proxy_request.hpp"
#include "util/http_query.hpp"
#include "util/json.hpp"
#include "util/json_convert.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"
#include "util/user_input.hpp"
#include "revlm_entities-odb.hxx"

#include <cstdint>
#include <date/date.h>
#include <date/tz.h>
#include <exception>
#include <httplib.h>
#include <odb/mysql/query.hxx>
#include <odb/nullable.hxx>
#include <odb/query.hxx>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revlm
{
namespace
{

struct ParsedRequest {
    std::string_view method;
    std::string_view path;
    std::string_view target;
    size_t header_bytes = 0;
    size_t content_length = 0;
    bool invalid_framing = false;
};

struct RequestContext {
    ParsedRequest parsed;
    std::string raw_request;
    std::string request_id;
    long long usage_event_id = 0;
    std::string client_ip;
    std::string set_cookie;
};

std::string serialize_json_http_bytes(int status, std::string_view reason, const json &body,
                                      std::string_view request_id)
{
    const std::string payload = serialize(body);
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
        << "Content-Type: application/json; charset=utf-8\r\n"
        << "Content-Length: " << payload.size() << "\r\n"
        << "X-Request-Id: " << request_id << "\r\n"
        << "Connection: close\r\n"
        << "\r\n";
    std::string bytes = out.str();
    bytes.append(payload);
    return bytes;
}

std::string build_raw_http_request(const ::httplib::Request &req)
{
    std::ostringstream out;
    out << req.method << ' ' << req.target << " HTTP/1.1\r\n";
    for (const auto &header : req.headers) {
        out << header.first << ": " << header.second << "\r\n";
    }
    out << "\r\n" << req.body;
    return out.str();
}

long long make_usage_event_id()
{
    using clock = std::chrono::steady_clock;
    static std::atomic_uint64_t seq{ 0 };
    const auto ns = static_cast<uint64_t>(clock::now().time_since_epoch().count());
    const uint64_t mixed = (ns << 16) ^ (seq.fetch_add(1) & 0xffffULL);
    return static_cast<long long>(mixed & 0x7fffffffffffffffULL);
}

std::string generate_request_id()
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "req_%llx", static_cast<unsigned long long>(make_usage_event_id()));
    return std::string{ buf };
}

// Correlation id, not a client contract: OpenAI/Anthropic return it in the response, never require it in.
// Honor a client-supplied id (X-Request-Id, then legacy x-client-request-id); otherwise mint one server-side
// so it is always present for logging/persistence. Oversized ids are untrusted -> replaced, never rejected.
std::string resolve_request_id(const ::httplib::Request &req)
{
    std::string id = trim_ascii(req.get_header_value("X-Request-Id"));
    if (id.empty())
        id = trim_ascii(req.get_header_value("x-client-request-id"));
    return (id.empty() || id.size() > 128) ? generate_request_id() : id;
}

json unauthorized_token_response()
{
    return json{ { "error", json{ { "message", "Unauthorized" } } } };
}

std::optional<std::string> extract_api_token(const ::httplib::Request &req)
{
    std::string authorization = trim_ascii(req.get_header_value("Authorization"));
    if (!authorization.empty()) {
        const size_t sep = authorization.find(' ');
        if (sep != std::string::npos) {
            const std::string scheme = lowercase_ascii(trim_ascii(authorization.substr(0, sep)));
            const std::string token = trim_ascii(authorization.substr(sep + 1));
            if (scheme == "bearer" && !token.empty()) {
                return token;
            }
        }
    }
    const std::string api_key = trim_ascii(req.get_header_value("x-api-key"));
    if (!api_key.empty()) {
        return api_key;
    }
    return std::nullopt;
}

// Returns channel_group_id; writes user_id / token_id. Nullopt on auth failure.
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

json list_user_tokens_response(const User &user, std::string_view request_id)
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

json create_user_token_response(std::string_view raw_request, std::string_view body, std::string_view request_id,
                                std::string *set_cookie)
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

json reveal_user_token_response(std::string_view raw_request, std::string_view request_id, long long token_id,
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

json rotate_user_token_response(std::string_view raw_request, std::string_view request_id, long long token_id,
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

json revoke_user_token_response(std::string_view raw_request, std::string_view request_id, long long token_id,
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
    try {
        UserStore &users = UserStore::instance();
        TokenStore &store = users.tokens();
        store.revoke_user_token(user->id, token_id);
        return json({ { "success", true } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "撤销失败" } });
    }
}

json delete_user_token_response(std::string_view raw_request, std::string_view request_id, long long token_id,
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

json token_channel_response(std::string_view raw_request, std::string_view request_id, long long token_id,
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

json set_token_channel_response(std::string_view raw_request, std::string_view request_id, long long token_id,
                                std::string_view body, std::string *set_cookie)
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

json token_models_response(long long channel_group_id)
{
    try {
        json body;
        body["object"] = "list";
        body["data"] = json::array();
        std::vector<std::string> seen;
        const ChannelGroup group = ChannelGroupStore::instance().get_channel_group_by_id(channel_group_id);
        if (group.status) {
            for (const Channel &channel : group.channels) {
                if (!channel.status) {
                    continue;
                }
                for (const Model &item : channel.models) {
                    std::string id = trim_ascii(item.name);
                    if (id.empty() || std::find(seen.begin(), seen.end(), id) != seen.end()) {
                        continue;
                    }
                    seen.push_back(id);
                    body["data"].push_back(json({ { "id", id },
                                                  { "object", "model" },
                                                  { "created", 0 },
                                                  { "owned_by", item.owned_by.empty() ? "revlm" : item.owned_by } }));
                }
            }
        }
        return std::move(body);
    } catch (const std::exception &) {
        throw std::runtime_error("查询模型目录失败");
    }
}

json token_model_retrieve_response(std::string_view requested_model_id, long long channel_group_id, bool &not_found)
{
    not_found = false;
    const std::string response_id = trim_ascii(requested_model_id);
    if (response_id.empty()) {
        not_found = true;
        return json{ { "error", json{ { "message", "not found" } } } };
    }

    try {
        const ChannelGroup group = ChannelGroupStore::instance().get_channel_group_by_id(channel_group_id);
        if (group.status) {
            for (const Channel &channel : group.channels) {
                if (!channel.status) {
                    continue;
                }
                if (const Model *model = channel.find_model(response_id)) {
                    return json({ { "id", response_id },
                                  { "object", "model" },
                                  { "created", 0 },
                                  { "owned_by", model->owned_by.empty() ? "revlm" : model->owned_by } });
                }
            }
        }
        not_found = true;
        return json{ { "error", json{ { "message", "not found" } } } };
    } catch (const std::exception &) {
        throw std::runtime_error("查询模型目录失败");
    }
}

json admin_users_json(std::vector<User> users)
{
    std::sort(users.begin(), users.end(), [](const User &a, const User &b) { return a.id > b.id; });
    json data = json::array();
    for (const User &user : users) {
        data.push_back(to_json(user));
    }
    return data;
}

json admin_list_users_response(std::string_view raw_request, std::string_view request_id, std::string *set_cookie)
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

json admin_create_user_response(std::string_view raw_request, std::string_view body, std::string_view request_id,
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
                                std::string_view request_id, std::string *set_cookie)
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
                                        std::string_view request_id, std::string *set_cookie)
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
                                     std::string_view request_id, std::string *set_cookie)
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

json admin_delete_user_response(long long user_id, std::string_view raw_request, std::string_view request_id,
                                std::string *set_cookie)
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

json billing_balance_response(std::string_view raw_request, std::string_view request_id, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    try {
        UserStore &store = UserStore::instance();
        return json(
            { { "success", true }, { "data", json{ { "balance_usd", store.get_user_balance_usd(user->id) } } } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

ParsedRequest parsed_request_from_httplib(const ::httplib::Request &req)
{
    ParsedRequest parsed;
    parsed.method = req.method;
    parsed.target = req.target;
    parsed.path = req.path;
    parsed.content_length = req.body.size();
    size_t header_bytes = req.method.size() + req.target.size() + 12;
    for (const auto &header : req.headers) {
        header_bytes += header.first.size() + header.second.size() + 4;
    }
    parsed.header_bytes = header_bytes + 4;
    return parsed;
}

bool validate_parsed_request(const ParsedRequest &parsed, std::string_view request_id, ::httplib::Response &res)
{
    if (parsed.header_bytes > static_cast<size_t>(config().http_max_header_bytes)) {
        write_json(res, 431, json("request header too large"), request_id);
        return false;
    }
    if (parsed.content_length > static_cast<size_t>(config().http_max_body_bytes)) {
        write_json(res, 413, json("payload too large"), request_id);
        return false;
    }
    return true;
}

RequestContext make_request_context(const ::httplib::Request &req)
{
    std::string request_id = resolve_request_id(req);
    const std::string client_ip = req.remote_addr.empty() ? "127.0.0.1" : req.remote_addr;
    return RequestContext{
        .parsed = parsed_request_from_httplib(req),
        .raw_request = inject_request_metadata(build_raw_http_request(req), client_ip),
        .request_id = std::move(request_id),
        .usage_event_id = make_usage_event_id(),
        .client_ip = client_ip,
    };
}

void log_access(std::string_view request_id, std::string_view method, std::string_view path, int status)
{
    std::cerr << "access request_id=" << request_id << " status=" << status << " method=" << method
              << " path=" << redact_request_target(path) << '\n';
}

::httplib::Server::Handler
make_http_handler(std::function<void(const ::httplib::Request &, ::httplib::Response &, RequestContext &)> handler)
{
    return [handler = std::move(handler)](const ::httplib::Request &req, ::httplib::Response &res) {
        RequestContext ctx = make_request_context(req);
        if (!validate_parsed_request(ctx.parsed, ctx.request_id, res)) {
            log_access(ctx.request_id, ctx.parsed.method, ctx.parsed.target, res.status);
            return;
        }
        handler(req, res, ctx);
        log_access(ctx.request_id, ctx.parsed.method, ctx.parsed.target, res.status);
    };
}

::httplib::Server::Handler
make_response_handler(std::function<json(const ::httplib::Request &, RequestContext &)> handler)
{
    return make_http_handler(
        [handler = std::move(handler)](const ::httplib::Request &req, ::httplib::Response &res, RequestContext &ctx) {
            write_json(res, 200, handler(req, ctx), ctx.request_id, ctx.set_cookie);
        });
}

std::optional<long long> path_param_i64(const ::httplib::Request &req, std::string_view name)
{
    const auto it = req.path_params.find(std::string{ name });
    if (it == req.path_params.end()) {
        return std::nullopt;
    }
    return parse_positive_i64_or(it->second);
}

std::string path_param_string(const ::httplib::Request &req, std::string_view name)
{
    const auto it = req.path_params.find(std::string{ name });
    return it == req.path_params.end() ? std::string{} : it->second;
}

class InMemoryHttpServer final : public ::httplib::Server {
public:
    bool process(::httplib::Stream &stream, const std::function<void(::httplib::Request &)> &setup_request)
    {
        bool connection_closed = false;
        // Ubuntu 24.04 ships cpp-httplib 0.14.3 (4-arg). 0.25+ define VERSION_NUM and use addr args.
#ifdef CPPHTTPLIB_VERSION_NUM
        return process_request(stream, "127.0.0.1", 0, "127.0.0.1", 0, true, connection_closed, setup_request);
#else
        return process_request(stream, true, connection_closed, setup_request);
#endif
    }
};

// Usage / request analytics handlers (merged from former usage/*_api.cpp).
constexpr std::string_view kAdminTimeZone = "Asia/Shanghai";

struct UsageQueryOptions {
    std::string time_zone = "UTC";
    bool all_time = false;
    std::optional<sys_seconds> start_utc;
    std::optional<sys_seconds> end_exclusive_utc;
    std::optional<long long> token_id;
};

std::optional<std::string> nullable_odb_string(const odb::nullable<std::string> &value)
{
    if (value.null() || value->empty()) {
        return std::nullopt;
    }
    return *value;
}

std::string model_icon_url(std::string_view owned_by)
{
    const std::string owner = std::string{ trim_ascii(owned_by) };
    if (owner.empty()) {
        return {};
    }
    return "/assets/model-icons/" + owner +
           (owner == "openai" || owner == "xai" || owner == "openrouter" || owner == "ollama" ? ".svg" : "-color.svg");
}

bool parse_usage_query_options(const std::map<std::string, std::string> &params, UsageQueryOptions &out,
                               std::string &message)
{
    out = UsageQueryOptions{};
    out.time_zone = trim_ascii(query_param_value(params, "tz"));
    if (out.time_zone.empty()) {
        out.time_zone = "UTC";
    }
    if (!zone_exists(out.time_zone)) {
        message = "tz 无效";
        return false;
    }

    const std::string all_time_raw = trim_ascii(query_param_value(params, "all_time"));
    if (!all_time_raw.empty()) {
        bool all_time = false;
        if (!parse_bool_flag(all_time_raw, all_time)) {
            message = "all_time 无效";
            return false;
        }
        out.all_time = all_time;
    }

    const std::string start = trim_ascii(query_param_value(params, "start"));
    const std::string end = trim_ascii(query_param_value(params, "end"));
    if (!start.empty()) {
        int y = 0;
        int m = 0;
        int d = 0;
        if (!parse_date_yyyy_mm_dd(start, y, m, d)) {
            message = "start 无效";
            return false;
        }
        out.start_utc = local_date_to_utc(y, static_cast<unsigned>(m), static_cast<unsigned>(d), out.time_zone);
    }
    if (!end.empty()) {
        int y = 0;
        int m = 0;
        int d = 0;
        if (!parse_date_yyyy_mm_dd(end, y, m, d)) {
            message = "end 无效";
            return false;
        }
        unsigned um = static_cast<unsigned>(m);
        unsigned ud = static_cast<unsigned>(d);
        const sys_seconds end_start = local_date_to_utc(y, um, ud, out.time_zone);
        next_date(y, um, ud);
        out.end_exclusive_utc = local_date_to_utc(y, um, ud, out.time_zone);
        if (*out.end_exclusive_utc <= end_start) {
            out.end_exclusive_utc = end_start + std::chrono::seconds{ 86400 };
        }
    }
    if (out.start_utc.has_value() && out.end_exclusive_utc.has_value() && *out.start_utc >= *out.end_exclusive_utc) {
        message = "日期范围无效";
        return false;
    }

    const std::string token_id_raw = trim_ascii(query_param_value(params, "token_id"));
    if (!token_id_raw.empty()) {
        long long token_id = 0;
        if (!parse_i64(token_id_raw, token_id) || token_id <= 0) {
            message = "token_id 无效";
            return false;
        }
        out.token_id = token_id;
    }
    return true;
}

RequestListFilter filter_from_usage_options(long long user_id, const UsageQueryOptions &options)
{
    RequestListFilter filter;
    filter.user_id = user_id;
    if (options.token_id.has_value()) {
        filter.token_id = options.token_id;
    }
    if (!options.all_time) {
        if (options.start_utc.has_value()) {
            filter.start = to_mysql_datetime(*options.start_utc);
        }
        if (options.end_exclusive_utc.has_value()) {
            filter.end_exclusive = to_mysql_datetime(*options.end_exclusive_utc);
        }
    }
    return filter;
}

json request_to_user_event_json(const Request &req)
{
    json o = to_json(req);
    o["time"] = req.time.empty() ? std::string{} : to_iso8601z(parse_mysql_datetime(req.time));
    o["response_id"] = req.response_id.null() ? json(nullptr) : json(*req.response_id);
    o["channel_id"] = req.channel_id > 0 ? json(req.channel_id) : json(nullptr);
    o["model"] = req.model_name.null() || req.model_name->empty() ? json(nullptr) : json(*req.model_name);
    o["cache_creation_tokens"] = req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
    o["cost_usd"] = request_detail::decimal_to_string(req.solve_price());
    return o;
}

json aggregate_window(const std::vector<Request> &rows, const UsageQueryOptions &options)
{
    long long requests = 0;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_tokens = 0;
    long long cache_creation_tokens = 0;
    long long first_token_sum = 0;
    long long first_token_samples = 0;
    long long decode_tokens = 0;
    long long decode_latency_ms = 0;
    double used = 0.0;
    std::optional<sys_seconds> min_time;
    std::optional<sys_seconds> max_time;

    for (const Request &req : rows) {
        ++requests;
        input_tokens += req.input_tokens;
        output_tokens += req.output_tokens;
        cache_read_tokens += req.cache_read_tokens;
        cache_creation_tokens += req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
        used += req.solve_price();
        if (req.first_token_latency_ms > 0) {
            first_token_sum += req.first_token_latency_ms;
            ++first_token_samples;
        }
        if (req.latency_ms > req.first_token_latency_ms && req.output_tokens > 0) {
            decode_tokens += req.output_tokens;
            decode_latency_ms += req.latency_ms - req.first_token_latency_ms;
        }
        if (!req.time.empty()) {
            try {
                const sys_seconds tp = parse_mysql_datetime(req.time);
                if (!min_time.has_value() || tp < *min_time) {
                    min_time = tp;
                }
                if (!max_time.has_value() || tp > *max_time) {
                    max_time = tp;
                }
            } catch (const std::exception &) {
            }
        }
    }

    const long long tokens = input_tokens + output_tokens + cache_read_tokens + cache_creation_tokens;
    std::string since;
    std::string until;
    if (!options.all_time) {
        if (options.start_utc.has_value()) {
            since = to_iso8601z(*options.start_utc);
        }
        if (options.end_exclusive_utc.has_value()) {
            until = to_iso8601z(*options.end_exclusive_utc - std::chrono::seconds{ 1 });
        }
    }
    if (since.empty() && min_time.has_value()) {
        since = to_iso8601z(*min_time);
    }
    if (until.empty() && max_time.has_value()) {
        until = to_iso8601z(*max_time);
    }

    double minutes = 1.0;
    if (options.start_utc.has_value() && options.end_exclusive_utc.has_value()) {
        minutes = std::max(1.0, std::chrono::duration<double>(*options.end_exclusive_utc - *options.start_utc).count() /
                                    60.0);
    } else if (min_time.has_value() && max_time.has_value() && *max_time >= *min_time) {
        minutes = std::max(1.0, std::chrono::duration<double>(*max_time - *min_time).count() / 60.0 + 1.0 / 60.0);
    }

    json window;
    window["window"] = "custom";
    window["since"] = since;
    window["until"] = until;
    window["requests"] = requests;
    window["tokens"] = tokens;
    window["rpm"] = static_cast<long long>(std::llround(static_cast<double>(requests) / minutes));
    window["tpm"] = static_cast<long long>(std::llround(static_cast<double>(tokens) / minutes));
    window["input_tokens"] = input_tokens;
    window["output_tokens"] = output_tokens;
    window["cache_read_tokens"] = cache_read_tokens;
    window["cache_creation_tokens"] = cache_creation_tokens;
    window["cache_ratio"] = input_tokens > 0 ? static_cast<double>(cache_read_tokens + cache_creation_tokens) /
                                                   static_cast<double>(input_tokens) :
                                               0.0;
    window["first_token_samples"] = first_token_samples;
    window["avg_first_token_latency"] =
        first_token_samples > 0 ? static_cast<double>(first_token_sum) / static_cast<double>(first_token_samples) : 0.0;
    window["tokens_per_second"] =
        decode_latency_ms > 0 ? static_cast<double>(decode_tokens) * 1000.0 / static_cast<double>(decode_latency_ms) :
                                0.0;
    window["usd"] = request_detail::decimal_to_string(used);
    return window;
}

json usage_time_series(const std::vector<Request> &rows, const std::string &tz, std::string_view granularity)
{
    std::map<std::string, RequestTotal> buckets;
    for (const Request &req : rows) {
        if (req.time.empty()) {
            continue;
        }
        sys_seconds tp;
        try {
            tp = parse_mysql_datetime(req.time);
        } catch (const std::exception &) {
            continue;
        }
        const std::string bucket = granularity == "day" ? day_bucket(tp, tz) : hour_bucket(tp, tz);
        RequestTotal &total = buckets[bucket];
        ++total.requests;
        const long long cache_creation = req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
        total.input_tokens += req.input_tokens;
        total.output_tokens += req.output_tokens;
        total.cache_read_tokens += req.cache_read_tokens;
        total.cache_creation_tokens += cache_creation;
        total.tokens += req.input_tokens + req.output_tokens + req.cache_read_tokens + cache_creation;
        total.usd += req.solve_price();
        total.first_token_latency_sum += std::max(req.first_token_latency_ms, 0);
    }

    json points = json::array();
    for (const auto &[bucket, total] : buckets) {
        const long long cached = total.cache_read_tokens + total.cache_creation_tokens;
        json point;
        point["bucket"] = bucket;
        point["requests"] = total.requests;
        point["tokens"] = total.tokens;
        point["usd"] = total.usd;
        point["cache_ratio"] =
            total.input_tokens > 0 ? static_cast<double>(cached) / static_cast<double>(total.input_tokens) : 0.0;
        point["avg_first_token_latency"] = total.requests > 0 ? static_cast<double>(total.first_token_latency_sum) /
                                                                    static_cast<double>(total.requests) :
                                                                0.0;
        point["tokens_per_second"] = 0.0;
        points.push_back(std::move(point));
    }
    return points;
}

json dashboard_model_stats(const std::vector<Request> &rows)
{
    std::map<std::string, RequestTotal> by_model;
    for (const Request &req : rows) {
        const std::string model = req.model_name.null() ? "" : *req.model_name;
        RequestTotal &total = by_model[model];
        ++total.requests;
        total.tokens += req.input_tokens + req.output_tokens + req.cache_read_tokens + req.cache_creation_5m_tokens +
                        req.cache_creation_1h_tokens;
        total.usd += req.solve_price();
    }
    std::vector<std::pair<std::string, RequestTotal>> ranked(by_model.begin(), by_model.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        if (a.second.requests != b.second.requests) {
            return a.second.requests > b.second.requests;
        }
        return a.first < b.first;
    });
    if (ranked.size() > 12) {
        ranked.resize(12);
    }
    static constexpr const char *kColors[] = { "#3b82f6", "#22c55e", "#f59e0b", "#ef4444", "#8b5cf6", "#06b6d4" };
    json out = json::array();
    for (size_t i = 0; i < ranked.size(); ++i) {
        json o;
        o["model"] = ranked[i].first;
        const Model *found = nullptr;
        for (const Model &model : all_models) {
            if (model.name == ranked[i].first) {
                found = &model;
                break;
            }
        }
        const std::string icon = found != nullptr ? model_icon_url(found->owned_by) : "";
        if (icon.empty()) {
            o["icon_url"] = nullptr;
        } else {
            o["icon_url"] = icon;
        }
        o["color"] = kColors[i % (sizeof(kColors) / sizeof(kColors[0]))];
        o["requests"] = ranked[i].second.requests;
        o["tokens"] = ranked[i].second.tokens;
        o["usd"] = request_detail::decimal_to_string(ranked[i].second.usd);
        out.push_back(std::move(o));
    }
    return out;
}

json user_models_detail_http_response(std::string_view raw_request, std::string_view request_id,
                                      std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    json models_json = json::array();
    for (const Model &model : all_models) {
        json o;
        o["id"] = model.id;
        o["public_id"] = model.name;
        o["owned_by"] = model.owned_by;
        o["input_usd_per_1m"] = request_detail::price_string(model.input_price);
        o["output_usd_per_1m"] = request_detail::price_string(model.output_price);
        o["cache_read_input_usd_per_1m"] = request_detail::price_string(model.cache_read_price);
        o["cache_creation_input_usd_per_1m"] = request_detail::price_string(model.cache_creation_5m_price);
        o["cache_creation_1h_input_usd_per_1m"] = request_detail::price_string(model.cache_creation_1h_price);
        o["status"] = 1;
        o["icon_url"] = model_icon_url(model.owned_by);
        models_json.push_back(std::move(o));
    }
    return json({ { "success", true }, { "data", std::move(models_json) } });
}

json dashboard_http_response(std::string_view raw_request, std::string_view request_id, std::string_view target,
                             std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return json({ { "success", false }, { "message", message } });
    }

    const auto now = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto local = date::make_zoned(options.time_zone, now).get_local_time();
    const date::year_month_day ymd{ date::floor<date::days>(local) };
    int year = static_cast<int>(ymd.year());
    unsigned month = static_cast<unsigned>(ymd.month());
    unsigned day = static_cast<unsigned>(ymd.day());
    options.all_time = false;
    options.start_utc = local_date_to_utc(year, month, day, options.time_zone);
    next_date(year, month, day);
    options.end_exclusive_utc = local_date_to_utc(year, month, day, options.time_zone);

    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        const json today = aggregate_window(rows, options);
        json charts;
        charts["model_stats"] = dashboard_model_stats(rows);
        charts["time_series_stats"] = usage_time_series(rows, options.time_zone, "hour");
        json body;
        body["today_usage_usd"] = today["usd"];
        body["today_since"] = today["since"];
        body["today_until"] = today["until"];
        body["today_requests"] = today["requests"];
        body["today_tokens"] = today["tokens"];
        body["today_rpm"] = std::to_string(today["rpm"].as_int64().value_or(0));
        body["today_tpm"] = std::to_string(today["tpm"].as_int64().value_or(0));
        body["charts"] = std::move(charts);
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json usage_windows_http_response(std::string_view raw_request, std::string_view request_id, std::string_view target,
                                 std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return json({ { "success", false }, { "message", message } });
    }
    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        json body;
        body["time_zone"] = options.time_zone;
        body["now"] = to_iso8601z(date::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        json windows;
        windows.push_back(aggregate_window(rows, options));
        body["windows"] = std::move(windows);
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json requests_http_response(std::string_view raw_request, std::string_view request_id, std::string_view target,
                            std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return json({ { "success", false }, { "message", message } });
    }

    int limit = 50;
    const std::string limit_raw = trim_ascii(query_param_value(params, "limit"));
    if (!limit_raw.empty()) {
        int parsed = 0;
        if (parse_i32(limit_raw, parsed) && parsed > 0 && parsed <= 100) {
            limit = parsed;
        }
    }
    RequestListFilter filter = filter_from_usage_options(user->id, options);
    const std::string before_id_raw = trim_ascii(query_param_value(params, "before_id"));
    if (!before_id_raw.empty()) {
        long long before_id = 0;
        if (parse_i64(before_id_raw, before_id) && before_id > 0) {
            filter.before_id = before_id;
        }
    }
    const std::string q_model = trim_ascii(query_param_value(params, "q_model"));
    if (!q_model.empty()) {
        filter.model_like = q_model;
    }
    filter.limit = limit + 1;

    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        auto loaded = store.query(filter);
        const bool has_extra = static_cast<int>(loaded.size()) > limit;
        if (has_extra) {
            loaded.resize(static_cast<size_t>(limit));
        }
        json body;
        json events = json::array();
        for (const Request &req : loaded) {
            events.push_back(request_to_user_event_json(req));
        }
        body["events"] = std::move(events);
        if (has_extra && !loaded.empty()) {
            body["next_before_id"] = loaded.back().id;
        } else {
            body["next_before_id"] = nullptr;
        }
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json usage_timeseries_http_response(std::string_view raw_request, std::string_view request_id, std::string_view target,
                                    std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return json({ { "success", false }, { "message", message } });
    }
    std::string granularity = trim_ascii(query_param_value(params, "granularity"));
    if (granularity.empty()) {
        granularity = "day";
    }
    if (granularity != "hour" && granularity != "day") {
        return json({ { "success", false }, { "message", "granularity 无效" } });
    }
    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        json body;
        body["time_zone"] = options.time_zone;
        body["start"] = options.start_utc.has_value() ? json(to_iso8601z(*options.start_utc)) : json(nullptr);
        body["end"] = options.end_exclusive_utc.has_value() ?
                          json(to_iso8601z(*options.end_exclusive_utc - std::chrono::seconds{ 1 })) :
                          json(nullptr);
        body["granularity"] = granularity;
        body["points"] = usage_time_series(rows, options.time_zone, granularity);
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json usage_event_detail_http_response(std::string_view raw_request, std::string_view request_id, long long event_id,
                                      std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    if (event_id <= 0) {
        return json({ { "success", false }, { "message", "event_id 无效" } });
    }
    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto req = store.get_by_id(event_id);
        if (!req.has_value() || req->user_id != user->id) {
            return json({ { "success", false }, { "message", "事件不存在" } });
        }
        json body;
        body["event_id"] = req->id;
        body["pricing_breakdown"] = to_json(compute_pricing_breakdown(*req));
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

struct AdminUsageRange {
    sys_seconds since_utc{};
    sys_seconds until_utc{};
    std::string start;
    std::string end;
    std::string since_local;
    std::string until_local;
    bool all_time = false;
};

std::optional<AdminUsageRange> resolve_admin_usage_range(const std::map<std::string, std::string> &params,
                                                         sys_seconds now_utc, std::string &error)
{
    error.clear();
    AdminUsageRange out;
    const auto today_local = date::make_zoned(std::string{ kAdminTimeZone }, now_utc).get_local_time();
    const date::year_month_day today_ymd{ date::floor<date::days>(today_local) };
    const std::string today = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d");

    const std::string all_time_raw = query_param_value(params, "all_time");
    if (!all_time_raw.empty() && !parse_bool_flag(all_time_raw, out.all_time)) {
        error = "all_time 不合法";
        return std::nullopt;
    }

    std::string start = trim_ascii(query_param_value(params, "start"));
    std::string end = trim_ascii(query_param_value(params, "end"));
    if (out.all_time) {
        RequestStore &store = UserStore::instance().tokens().requests();
        RequestListFilter filter;
        filter.limit = 1;
        filter.order_asc = true;
        const auto first_rows = store.query(filter);
        if (!first_rows.empty() && !first_rows.front().time.empty()) {
            try {
                start = format_local(parse_mysql_datetime(first_rows.front().time), std::string{ kAdminTimeZone },
                                     "%Y-%m-%d");
                end = today;
            } catch (const std::exception &) {
                start.clear();
                end.clear();
            }
        } else {
            start.clear();
            end.clear();
        }
    }
    if (start.empty()) {
        start = today;
    }
    if (end.empty()) {
        end = start;
    }

    int start_y = 0;
    int start_m = 0;
    int start_d = 0;
    int end_y = 0;
    int end_m = 0;
    int end_d = 0;
    if (!parse_date_yyyy_mm_dd(start, start_y, start_m, start_d)) {
        error = "start 不合法（格式：YYYY-MM-DD）";
        return std::nullopt;
    }
    if (!parse_date_yyyy_mm_dd(end, end_y, end_m, end_d)) {
        error = "end 不合法（格式：YYYY-MM-DD）";
        return std::nullopt;
    }
    unsigned sm = static_cast<unsigned>(start_m);
    unsigned sd = static_cast<unsigned>(start_d);
    unsigned em = static_cast<unsigned>(end_m);
    unsigned ed = static_cast<unsigned>(end_d);
    out.since_utc = local_date_to_utc(start_y, sm, sd, std::string{ kAdminTimeZone });
    const sys_seconds end_start = local_date_to_utc(end_y, em, ed, std::string{ kAdminTimeZone });
    next_date(end_y, em, ed);
    const sys_seconds end_exclusive = local_date_to_utc(end_y, em, ed, std::string{ kAdminTimeZone });
    if (out.since_utc >= end_exclusive) {
        error = "start 不能晚于 end";
        return std::nullopt;
    }
    out.start = start;
    out.end = end;
    out.since_local = format_local(out.since_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d %H:%M");
    const sys_seconds today_start =
        local_date_to_utc(static_cast<int>(today_ymd.year()), static_cast<unsigned>(today_ymd.month()),
                          static_cast<unsigned>(today_ymd.day()), std::string{ kAdminTimeZone });
    if (end_start >= today_start) {
        out.end = today;
        out.until_utc = now_utc;
        out.until_local = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d %H:%M");
    } else {
        out.until_utc = end_exclusive;
        out.until_local =
            format_local(end_exclusive - std::chrono::seconds{ 1 }, std::string{ kAdminTimeZone }, "%Y-%m-%d %H:%M");
    }
    return out;
}

RequestListFilter build_admin_filter(const std::map<std::string, std::string> &params, const AdminUsageRange &range,
                                     int limit, std::string &error)
{
    odb::database &db = database();
    error.clear();
    RequestListFilter filters;
    filters.limit = limit;
    filters.start = to_mysql_datetime(range.since_utc);
    filters.end_exclusive = to_mysql_datetime(range.until_utc);

    const std::string user_id_raw = trim_ascii(query_param_value(params, "user_id"));
    if (!user_id_raw.empty()) {
        long long user_id = 0;
        if (!parse_i64(user_id_raw, user_id) || user_id <= 0) {
            error = "user_id 不合法";
            return filters;
        }
        filters.user_id = user_id;
    }
    const std::string channel_id_raw = trim_ascii(query_param_value(params, "channel_id"));
    if (!channel_id_raw.empty()) {
        long long channel_id = 0;
        if (!parse_i64(channel_id_raw, channel_id) || channel_id <= 0) {
            error = "channel_id 不合法";
            return filters;
        }
        filters.channel_id = channel_id;
    }
    const std::string model = trim_ascii(query_param_value(params, "model"));
    if (!model.empty()) {
        filters.model_exact = model;
    }
    const std::string q_model = trim_ascii(query_param_value(params, "q_model"));
    if (!q_model.empty()) {
        filters.model_like = q_model;
    }

    const std::string q_user = trim_ascii(query_param_value(params, "q_user"));
    if (!q_user.empty()) {
        using uq = odb::query<User>;
        ScopedTransaction t(db);
        for (const User &u :
             db.query<User>(uq::email.like("%" + q_user + "%") || uq::username.like("%" + q_user + "%"))) {
            filters.user_ids.push_back(u.id);
        }
        t.commit();
        if (filters.user_ids.empty()) {
            filters.user_ids.push_back(-1);
        }
    }
    const std::string q_channel = trim_ascii(query_param_value(params, "q_channel"));
    if (!q_channel.empty()) {
        using cq = odb::query<Channel>;
        ScopedTransaction t(db);
        for (const Channel &c : db.query<Channel>(cq::name.like("%" + q_channel + "%"))) {
            filters.channel_ids.push_back(c.id);
        }
        t.commit();
        if (filters.channel_ids.empty()) {
            filters.channel_ids.push_back(-1);
        }
    }

    const std::string before_id_raw = trim_ascii(query_param_value(params, "before_id"));
    if (!before_id_raw.empty()) {
        long long before_id = 0;
        if (!parse_i64(before_id_raw, before_id) || before_id <= 0) {
            error = "before_id 不合法";
            return filters;
        }
        filters.before_id = before_id;
    }
    const std::string after_id_raw = trim_ascii(query_param_value(params, "after_id"));
    if (!after_id_raw.empty()) {
        long long after_id = 0;
        if (!parse_i64(after_id_raw, after_id) || after_id <= 0) {
            error = "after_id 不合法";
            return filters;
        }
        filters.after_id = after_id;
        filters.order_asc = true;
    }
    if (filters.before_id.has_value() && filters.after_id.has_value()) {
        error = "before_id 与 after_id 不能同时使用";
    }
    return filters;
}

json request_to_admin_event_json(const Request &req, std::string_view user_email, std::string_view channel_name)
{
    const long long cached_tokens = req.cache_read_tokens + req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
    json o = to_json(req);
    o["time"] = req.time.empty() ? std::string{} : to_iso8601z(parse_mysql_datetime(req.time));
    o["user_email"] = user_email;
    o["model"] = req.model_name.null() ? json(nullptr) : json(*req.model_name);
    if (req.output_tokens > 0 && req.latency_ms > 0) {
        o["tokens_per_second"] = request_detail::decimal_to_string(static_cast<double>(req.output_tokens) * 1000.0 /
                                                                   static_cast<double>(req.latency_ms));
    } else {
        o["tokens_per_second"] = "-";
    }
    o["cached_tokens"] = cached_tokens;
    o["cost_usd"] = request_detail::decimal_to_string(req.solve_price());
    o["upstream_channel_name"] = channel_name;
    o["response_id"] = req.response_id.null() ? json(nullptr) : json(*req.response_id);
    const auto error_class = nullable_odb_string(req.error_class);
    const auto error_message = nullable_odb_string(req.error_message);
    std::string error;
    if (error_class.has_value() && error_message.has_value()) {
        error = *error_class + " (" + *error_message + ")";
    } else if (error_class.has_value()) {
        error = *error_class;
    } else if (error_message.has_value()) {
        error = *error_message;
    }
    o["error"] = error;
    return o;
}

json admin_window_summary(const AdminUsageRange &range, const std::vector<Request> &rows,
                          const std::vector<Request> &recent_rows)
{
    long long requests = 0;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_tokens = 0;
    long long cache_creation_tokens = 0;
    long long first_token_sum = 0;
    long long first_token_samples = 0;
    long long decode_tokens = 0;
    long long decode_latency_ms = 0;
    double used = 0.0;
    for (const Request &req : rows) {
        ++requests;
        input_tokens += req.input_tokens;
        output_tokens += req.output_tokens;
        cache_read_tokens += req.cache_read_tokens;
        cache_creation_tokens += req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
        used += req.solve_price();
        if (req.first_token_latency_ms > 0) {
            first_token_sum += req.first_token_latency_ms;
            ++first_token_samples;
        }
        if (req.output_tokens > 0 && req.latency_ms > req.first_token_latency_ms) {
            decode_tokens += req.output_tokens;
            decode_latency_ms += req.latency_ms - req.first_token_latency_ms;
        }
    }
    long long recent_requests = 0;
    long long recent_tokens = 0;
    for (const Request &req : recent_rows) {
        ++recent_requests;
        recent_tokens += req.input_tokens + req.output_tokens;
    }
    const double total_tokens = static_cast<double>(input_tokens + output_tokens);
    const double cached_tokens = static_cast<double>(cache_read_tokens + cache_creation_tokens);
    json o;
    o["window"] = "统计区间";
    o["since"] = range.since_local;
    o["until"] = range.until_local;
    o["requests"] = requests;
    o["tokens"] = input_tokens + output_tokens;
    o["input_tokens"] = input_tokens;
    o["output_tokens"] = output_tokens;
    o["cached_tokens"] = cache_read_tokens + cache_creation_tokens;
    o["cache_ratio"] =
        request_detail::decimal_to_string((total_tokens > 0 ? cached_tokens / total_tokens : 0.0) * 100.0);
    o["rpm"] = request_detail::decimal_to_string(static_cast<double>(recent_requests));
    o["tpm"] = request_detail::decimal_to_string(static_cast<double>(recent_tokens));
    o["avg_first_token_latency"] = request_detail::decimal_to_string(
        first_token_samples > 0 ? static_cast<double>(first_token_sum) / static_cast<double>(first_token_samples) :
                                  0.0);
    o["tokens_per_second"] = request_detail::decimal_to_string(
        decode_latency_ms > 0 ? static_cast<double>(decode_tokens) * 1000.0 / static_cast<double>(decode_latency_ms) :
                                0.0);
    o["usd"] = request_detail::decimal_to_string(used);
    return o;
}

json top_users_json(const std::vector<Request> &rows)
{
    struct Acc {
        std::string email;
        std::string role;
        long long status = 0;
        double used = 0.0;
    };
    std::map<long long, Acc> by_user;
    UserStore &users = UserStore::instance();
    for (const Request &req : rows) {
        Acc &acc = by_user[req.user_id];
        if (acc.email.empty()) {
            const User u = users.get_user_by_id(req.user_id);
            acc.email = u.email;
            acc.role = u.role;
            acc.status = u.status;
        }
        acc.used += req.solve_price();
    }
    std::vector<std::pair<long long, Acc>> ranked(by_user.begin(), by_user.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        if (a.second.used != b.second.used) {
            return a.second.used > b.second.used;
        }
        return a.first > b.first;
    });
    if (ranked.size() > 50) {
        ranked.resize(50);
    }
    json out = json::array();
    for (const auto &entry : ranked) {
        json o;
        o["user_id"] = entry.first;
        o["email"] = entry.second.email;
        o["role"] = entry.second.role;
        o["status"] = entry.second.status;
        o["usd"] = request_detail::decimal_to_string(entry.second.used);
        out.push_back(std::move(o));
    }
    return out;
}

json admin_dashboard_http_response(std::string_view raw_request, std::string_view request_id, std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    try {
        UserStore &users = UserStore::instance();
        ChannelStore &channels = ChannelStore::instance();
        const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto local = date::make_zoned(std::string{ kAdminTimeZone }, now_utc).get_local_time();
        const date::year_month_day ymd{ date::floor<date::days>(local) };
        const sys_seconds today_start =
            local_date_to_utc(static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                              static_cast<unsigned>(ymd.day()), std::string{ kAdminTimeZone });

        RequestListFilter filter;
        filter.start = to_mysql_datetime(today_start);
        filter.end_exclusive = to_mysql_datetime(now_utc);
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filter);
        long long requests_today = 0;
        long long input_tokens = 0;
        long long output_tokens = 0;
        double cost = 0.0;
        for (const Request &req : rows) {
            ++requests_today;
            input_tokens += req.input_tokens;
            output_tokens += req.output_tokens;
            cost += req.solve_price();
        }
        json stats;
        stats["users_count"] = users.count_users();
        const auto channel_list = channels.list_channels();
        stats["channels_count"] = static_cast<long long>(channel_list.size());
        stats["endpoints_count"] = static_cast<long long>(channel_list.size());
        stats["requests_today"] = requests_today;
        stats["tokens_today"] = input_tokens + output_tokens;
        stats["input_tokens_today"] = input_tokens;
        stats["output_tokens_today"] = output_tokens;
        stats["cost_today"] = request_detail::decimal_to_string(cost);
        json data;
        data["admin_time_zone"] = kAdminTimeZone;
        data["stats"] = std::move(stats);
        return json({ { "success", true }, { "data", std::move(data) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "读取统计失败" } });
    }
}

json admin_usage_page_http_response(std::string_view raw_request, std::string_view request_id, std::string_view target,
                                    std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    const auto params = parse_query_map(target);
    int limit = 50;
    const std::string limit_raw = query_param_value(params, "limit");
    if (!limit_raw.empty() && !parse_i32(limit_raw, limit)) {
        return json({ { "success", false }, { "message", "limit 不合法" } });
    }
    if (limit < 10) {
        limit = 10;
    }
    if (limit > 200) {
        limit = 200;
    }
    bool include_summary = true;
    const std::string summary_raw = query_param_value(params, "summary");
    if (!summary_raw.empty() && !parse_bool_flag(summary_raw, include_summary)) {
        return json({ { "success", false }, { "message", "summary 不合法" } });
    }

    try {
        const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        std::string range_error;
        const auto range = resolve_admin_usage_range(params, now_utc, range_error);
        if (!range.has_value()) {
            return json({ { "success", false }, { "message", range_error } });
        }
        std::string filter_error;
        RequestListFilter page_filter = build_admin_filter(params, *range, limit + 1, filter_error);
        if (!filter_error.empty()) {
            return json({ { "success", false }, { "message", filter_error } });
        }
        RequestStore &store = UserStore::instance().tokens().requests();
        auto loaded = store.query(page_filter);
        const bool after = page_filter.after_id.has_value();
        if (after) {
            std::reverse(loaded.begin(), loaded.end());
        }
        const bool has_extra = static_cast<int>(loaded.size()) > limit;
        if (has_extra) {
            loaded.resize(static_cast<size_t>(limit));
        }

        std::map<long long, std::string> emails;
        std::map<long long, std::string> channel_names;
        UserStore &users = UserStore::instance();
        ChannelStore &channels = ChannelStore::instance();
        for (const Channel &c : channels.list_channels()) {
            channel_names[c.id] = c.name;
        }

        json events = json::array();
        for (const Request &req : loaded) {
            if (!emails.contains(req.user_id)) {
                emails[req.user_id] = users.get_user_by_id(req.user_id).email;
            }
            events.push_back(request_to_admin_event_json(
                req, emails[req.user_id], channel_names.contains(req.channel_id) ? channel_names[req.channel_id] : ""));
        }

        json data;
        data["admin_time_zone"] = kAdminTimeZone;
        data["now"] = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d %H:%M");
        data["start"] = range->start;
        data["end"] = range->end;
        data["limit"] = limit;
        data["events"] = std::move(events);
        if (has_extra && !loaded.empty()) {
            data["next_before_id"] = loaded.back().id;
        } else {
            data["next_before_id"] = nullptr;
        }
        if ((after || page_filter.before_id.has_value()) && !loaded.empty()) {
            data["prev_after_id"] = loaded.front().id;
        } else {
            data["prev_after_id"] = nullptr;
        }
        data["cursor_active"] = page_filter.before_id.has_value() || page_filter.after_id.has_value();

        if (include_summary) {
            RequestListFilter summary_filter = page_filter;
            summary_filter.limit = 0;
            summary_filter.before_id.reset();
            summary_filter.after_id.reset();
            summary_filter.order_asc = false;
            const auto summary_rows = store.query(summary_filter);
            RequestListFilter recent_filter;
            recent_filter.start = to_mysql_datetime(now_utc - std::chrono::seconds{ 60 });
            recent_filter.end_exclusive = to_mysql_datetime(now_utc + std::chrono::seconds{ 1 });
            const auto recent_rows = store.query(recent_filter);
            data["window"] = admin_window_summary(*range, summary_rows, recent_rows);
            data["top_users"] = top_users_json(summary_rows);
        }
        return json({ { "success", true }, { "data", std::move(data) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json admin_usage_event_detail_http_response(std::string_view raw_request, std::string_view request_id,
                                            long long event_id, std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    if (event_id <= 0) {
        return json({ { "success", false }, { "message", "event_id 不合法" } });
    }
    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto req = store.get_by_id(event_id);
        if (!req.has_value()) {
            return json({ { "success", false }, { "message", "not found" } });
        }
        json body;
        body["event_id"] = req->id;
        body["pricing_breakdown"] = to_json(compute_pricing_breakdown(*req));
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "查询失败" } });
    }
}

json admin_usage_timeseries_http_response(std::string_view raw_request, std::string_view request_id,
                                          std::string_view target, std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    std::map<std::string, std::string> params = parse_query_map(target);
    std::string granularity = std::string{ trim_ascii(query_param_value(params, "granularity")) };
    if (granularity.empty()) {
        granularity = "hour";
    }
    if (granularity != "hour" && granularity != "day") {
        return json({ { "success", false }, { "message", "granularity 仅支持 hour/day" } });
    }
    bool all_time = false;
    const std::string all_time_raw = query_param_value(params, "all_time");
    if (!all_time_raw.empty() && !parse_bool_flag(all_time_raw, all_time)) {
        return json({ { "success", false }, { "message", "all_time 不合法" } });
    }
    const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    if (query_param_value(params, "start").empty() && query_param_value(params, "end").empty() && !all_time) {
        if (granularity == "day") {
            params["start"] = format_local(now_utc - std::chrono::seconds{ 29 * 24 * 3600 },
                                           std::string{ kAdminTimeZone }, "%Y-%m-%d");
            params["end"] = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d");
        } else {
            params["start"] = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d");
            params["end"] = params["start"];
        }
    }
    try {
        std::string range_error;
        const auto range = resolve_admin_usage_range(params, now_utc, range_error);
        if (!range.has_value()) {
            return json({ { "success", false }, { "message", range_error } });
        }
        std::string filter_error;
        RequestListFilter filters = build_admin_filter(params, *range, 0, filter_error);
        if (!filter_error.empty()) {
            return json({ { "success", false }, { "message", filter_error } });
        }
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filters);
        json body;
        body["admin_time_zone"] = kAdminTimeZone;
        body["start"] = range->start;
        body["end"] = range->end;
        body["granularity"] = granularity;
        body["points"] = usage_time_series(rows, std::string{ kAdminTimeZone }, granularity);
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "查询失败" } });
    }
}

} // namespace

ProxyRequest make_request(const ::httplib::Request &req)
{
    ProxyRequest pr;
    pr.id = make_usage_event_id();
    pr.request_id = resolve_request_id(req);
    pr.time = to_mysql_datetime(std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()));
    pr.http.method = req.method;
    pr.http.path = req.path;
    pr.http.body = req.body;
    pr.http.client_ip = req.remote_addr.empty() ? "127.0.0.1" : req.remote_addr;
    for (const auto &entry : req.headers) {
        const std::string lower = lowercase_ascii(entry.first);
        if (lower == "authorization" || lower == "x-api-key") {
            continue;
        }
        pr.http.headers.emplace_back(entry.first, entry.second);
    }
    return pr;
}

void proxy_stream_commit_usage(ProxyRequest &pr)
{
    try {
        if (!commit_proxy_usage(pr)) {
            std::cerr << "stream usage commit failed\n";
        }
    } catch (const std::exception &err) {
        std::cerr << "stream usage callback failed: " << err.what() << '\n';
    }
}

void finish_proxy_usage(::httplib::Response &res, ProxyRequest &pr)
{
    (void)res;
    if (pr.upstream.channel_id <= 0) {
        return;
    }
    // Upstream body already written — never replace success with synthetic billing errors.
    if (!commit_proxy_usage(pr)) {
        std::cerr << "usage commit failed request_id=" << pr.request_id << '\n';
    }
}

void register_http_routes(::httplib::Server &server, const std::shared_ptr<std::atomic_bool> &draining)
{
    auto api = [](auto fn) {
        return make_response_handler(
            [fn = std::move(fn)](const ::httplib::Request &req, RequestContext &ctx) -> json { return fn(req, ctx); });
    };
    auto v1_http = [](auto fn) {
        return [fn = std::move(fn)](const ::httplib::Request &req, ::httplib::Response &res) {
            ProxyRequest pr = make_request(req);
            if (pr.http.body.size() > static_cast<size_t>(config().http_max_body_bytes)) {
                write_json(res, 413, json("payload too large"), pr.request_id);
                log_access(pr.request_id, pr.http.method, pr.http.path, res.status);
                return;
            }
            long long user_id = 0;
            long long token_id = 0;
            const auto channel_group_id = authenticate_api_token(req, user_id, token_id);
            if (!channel_group_id.has_value()) {
                write_json(res, 401, unauthorized_token_response(), pr.request_id);
                log_access(pr.request_id, pr.http.method, pr.http.path, res.status);
                return;
            }
            pr.auth.user_id = user_id;
            pr.auth.token_id = token_id;
            pr.auth.channel_group_id = *channel_group_id;
            fn(req, res, pr);
            log_access(pr.request_id, pr.http.method, pr.http.path, res.status);
        };
    };

    server.Get("/readyz",
               make_http_handler([draining](const ::httplib::Request &, ::httplib::Response &res, RequestContext &ctx) {
                   if (draining->load()) {
                       res.status = 503;
                       res.reason = "Service Unavailable";
                       res.set_header("X-Request-Id", ctx.request_id);
                       res.set_content("draining", "text/plain; charset=utf-8");
                       return;
                   }
                   res.status = 200;
                   res.reason = "OK";
                   res.set_header("X-Request-Id", ctx.request_id);
                   res.set_content("ok", "text/plain; charset=utf-8");
               }));
    server.Get("/api/user/self", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return self_response(ctx.raw_request, &ctx.set_cookie);
               }));
    server.Get("/api/user/logout", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return logout_response(ctx.raw_request, &ctx.set_cookie);
               }));
    server.Get("/api/user/models/detail", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return user_models_detail_http_response(ctx.raw_request, ctx.request_id, &ctx.set_cookie);
               }));
    server.Get("/api/dashboard", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return dashboard_http_response(ctx.raw_request, ctx.request_id, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/request/windows", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return usage_windows_http_response(ctx.raw_request, ctx.request_id, ctx.parsed.target,
                                                      &ctx.set_cookie);
               }));
    server.Get("/api/request/events", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return requests_http_response(ctx.raw_request, ctx.request_id, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/request/timeseries", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return usage_timeseries_http_response(ctx.raw_request, ctx.request_id, ctx.parsed.target,
                                                         &ctx.set_cookie);
               }));
    server.Get("/api/request/events/:event_id/detail", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto event_id = path_param_i64(req, "event_id");
                   if (!event_id.has_value()) {
                       return json({ { "success", false }, { "message", "event_id 无效" } });
                   }
                   return usage_event_detail_http_response(ctx.raw_request, ctx.request_id, *event_id, &ctx.set_cookie);
               }));
    server.Get("/api/token", api([](const ::httplib::Request &, RequestContext &ctx) {
                   json error;
                   const auto user = api_authenticated_user(ctx.raw_request, error, &ctx.set_cookie);
                   if (!user.has_value()) {
                       return error;
                   }
                   return list_user_tokens_response(*user, ctx.request_id);
               }));
    server.Post("/api/token", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return create_user_token_response(ctx.raw_request, req.body, ctx.request_id, &ctx.set_cookie);
                }));
    server.Get("/api/token/:token_id/reveal", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ?
                              reveal_user_token_response(ctx.raw_request, ctx.request_id, *token_id, &ctx.set_cookie) :
                              json({ { "success", false }, { "message", "token_id 不合法" } });
               }));
    server.Post("/api/token/:token_id/rotate", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto token_id = path_param_i64(req, "token_id");
                    return token_id.has_value() ?
                               rotate_user_token_response(ctx.raw_request, ctx.request_id, *token_id, &ctx.set_cookie) :
                               json({ { "success", false }, { "message", "token_id 不合法" } });
                }));
    server.Post("/api/token/:token_id/revoke", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto token_id = path_param_i64(req, "token_id");
                    return token_id.has_value() ?
                               revoke_user_token_response(ctx.raw_request, ctx.request_id, *token_id, &ctx.set_cookie) :
                               json({ { "success", false }, { "message", "token_id 不合法" } });
                }));
    server.Delete("/api/token/:token_id", api([](const ::httplib::Request &req, RequestContext &ctx) {
                      const auto token_id = path_param_i64(req, "token_id");
                      return token_id.has_value() ? delete_user_token_response(ctx.raw_request, ctx.request_id,
                                                                               *token_id, &ctx.set_cookie) :
                                                    json({ { "success", false }, { "message", "token_id 不合法" } });
                  }));
    server.Get("/api/token/:token_id/channel", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ?
                              token_channel_response(ctx.raw_request, ctx.request_id, *token_id, &ctx.set_cookie) :
                              json({ { "success", false }, { "message", "token_id 不合法" } });
               }));
    server.Put("/api/token/:token_id/channel", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ? set_token_channel_response(ctx.raw_request, ctx.request_id, *token_id,
                                                                            req.body, &ctx.set_cookie) :
                                                 json({ { "success", false }, { "message", "token_id 不合法" } });
               }));
    server.Post("/api/user/register", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return register_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Post("/api/user/login", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return login_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Post("/api/account/email", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return account_email_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Post("/api/account/password", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return account_password_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Get("/v1/models", v1_http([](const ::httplib::Request &, ::httplib::Response &res, ProxyRequest &pr) {
                   try {
                       write_json(res, 200, token_models_response(pr.auth.channel_group_id), pr.request_id);
                   } catch (const std::exception &) {
                       write_json(res, 502, json("查询模型目录失败"), pr.request_id);
                   }
               }));
    server.Get("/v1/models/:model_id",
               v1_http([](const ::httplib::Request &req, ::httplib::Response &res, ProxyRequest &pr) {
                   try {
                       bool not_found = false;
                       json body = token_model_retrieve_response(path_param_string(req, "model_id"),
                                                                 pr.auth.channel_group_id, not_found);
                       write_json(res, not_found ? 404 : 200, std::move(body), pr.request_id);
                   } catch (const std::exception &) {
                       write_json(res, 502, json("查询模型目录失败"), pr.request_id);
                   }
               }));
    server.Post("/v1/chat/completions",
                v1_http([](const ::httplib::Request &req, ::httplib::Response &res, ProxyRequest &pr) {
                    if (const auto quota_error = paygo_balance_gate(pr.auth.user_id); quota_error.has_value()) {
                        write_json(res, 402, *quota_error, pr.request_id);
                        return;
                    }
                    pr.is_stream = parse_json_bool_field(req.body, "stream").value_or(false);
                    if (pr.is_stream) {
                        run_chat_completions_stream(res, std::move(pr), proxy_stream_commit_usage);
                        return;
                    }
                    write_proxy_result(res, run_chat_completions(pr));
                    finish_proxy_usage(res, pr);
                }));
    server.Post("/v1/messages", v1_http([](const ::httplib::Request &req, ::httplib::Response &res, ProxyRequest &pr) {
                    if (const auto quota_error = paygo_balance_gate(pr.auth.user_id); quota_error.has_value()) {
                        write_json(res, 402, *quota_error, pr.request_id);
                        return;
                    }
                    pr.is_stream = parse_json_bool_field(req.body, "stream").value_or(false);
                    if (pr.is_stream) {
                        run_messages_stream(res, std::move(pr), proxy_stream_commit_usage);
                        return;
                    }
                    write_proxy_result(res, run_messages(pr));
                    finish_proxy_usage(res, pr);
                }));
    server.Post("/v1/responses", v1_http([](const ::httplib::Request &req, ::httplib::Response &res, ProxyRequest &pr) {
                    if (const auto quota_error = paygo_balance_gate(pr.auth.user_id); quota_error.has_value()) {
                        write_json(res, 402, *quota_error, pr.request_id);
                        return;
                    }
                    pr.is_stream = parse_json_bool_field(req.body, "stream").value_or(false);
                    ResponsesProxyExecuteOptions options;
                    if (pr.is_stream) {
                        options.stream_response = &res;
                        options.on_usage = proxy_stream_commit_usage;
                    }
                    auto result = handle_responses_proxy_request(pr, res, options);
                    if (!result.handled_stream) {
                        finish_proxy_usage(res, pr);
                    }
                }));
    server.Post("/v1/responses/input_tokens",
                v1_http([](const ::httplib::Request &req, ::httplib::Response &res, ProxyRequest &pr) {
                    if (const auto quota_error = paygo_balance_gate(pr.auth.user_id); quota_error.has_value()) {
                        write_json(res, 402, *quota_error, pr.request_id);
                        return;
                    }
                    handle_responses_proxy_request(pr, res);
                    finish_proxy_usage(res, pr);
                }));

    server.Get("/api/admin/dashboard", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return admin_dashboard_http_response(ctx.raw_request, ctx.request_id, &ctx.set_cookie);
               }));
    server.Get("/api/admin/request", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return admin_usage_page_http_response(ctx.raw_request, ctx.request_id, ctx.parsed.target,
                                                         &ctx.set_cookie);
               }));
    server.Get("/api/admin/request/timeseries", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return admin_usage_timeseries_http_response(ctx.raw_request, ctx.request_id, ctx.parsed.target,
                                                               &ctx.set_cookie);
               }));
    server.Get("/api/admin/request/events/:event_id/detail",
               api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto event_id = path_param_i64(req, "event_id");
                   return event_id.has_value() ? admin_usage_event_detail_http_response(ctx.raw_request, ctx.request_id,
                                                                                        *event_id, &ctx.set_cookie) :
                                                 json({ { "success", false }, { "message", "event_id 无效" } });
               }));
    server.Get("/api/admin/users", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return admin_list_users_response(ctx.raw_request, ctx.request_id, &ctx.set_cookie);
               }));
    server.Post("/api/admin/users", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return admin_create_user_response(ctx.raw_request, req.body, ctx.request_id, &ctx.set_cookie);
                }));
    server.Put("/api/admin/users/:user_id", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto user_id = path_param_i64(req, "user_id");
                   return user_id.has_value() ? admin_update_user_response(*user_id, ctx.raw_request, req.body,
                                                                           ctx.request_id, &ctx.set_cookie) :
                                                json({ { "success", false }, { "message", "用户不存在" } });
               }));
    server.Delete("/api/admin/users/:user_id", api([](const ::httplib::Request &req, RequestContext &ctx) {
                      const auto user_id = path_param_i64(req, "user_id");
                      return user_id.has_value() ? admin_delete_user_response(*user_id, ctx.raw_request, ctx.request_id,
                                                                              &ctx.set_cookie) :
                                                   json({ { "success", false }, { "message", "用户不存在" } });
                  }));
    server.Post("/api/admin/users/:user_id/password", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto user_id = path_param_i64(req, "user_id");
                    return user_id.has_value() ? admin_reset_user_password_response(*user_id, ctx.raw_request, req.body,
                                                                                    ctx.request_id, &ctx.set_cookie) :
                                                 json({ { "success", false }, { "message", "用户不存在" } });
                }));
    server.Post("/api/admin/users/:user_id/balance", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto user_id = path_param_i64(req, "user_id");
                    return user_id.has_value() ? admin_add_user_balance_response(*user_id, ctx.raw_request, req.body,
                                                                                 ctx.request_id, &ctx.set_cookie) :
                                                 json({ { "success", false }, { "message", "用户不存在" } });
                }));

    server.Get("/api/billing/balance", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return billing_balance_response(ctx.raw_request, ctx.request_id, &ctx.set_cookie);
               }));

    auto channel_groups = api([](const ::httplib::Request &req, RequestContext &ctx) {
        const ChannelGroupsParsedRequest parsed{ ctx.parsed.method, ctx.parsed.path, ctx.parsed.target };
        return channel_groups_route(ctx.raw_request, req.body, parsed, &ctx.set_cookie);
    });
    server.Get(R"(/api/admin/channel-groups.*)", channel_groups);
    server.Post(R"(/api/admin/channel-groups.*)", channel_groups);
    server.Put(R"(/api/admin/channel-groups.*)", channel_groups);
    server.Delete(R"(/api/admin/channel-groups.*)", channel_groups);

    auto channels = api([](const ::httplib::Request &req, RequestContext &ctx) {
        const ChannelParsedRequest parsed{ ctx.parsed.method, ctx.parsed.path, ctx.parsed.target };
        return channel_route(ctx.raw_request, req.body, parsed, &ctx.set_cookie);
    });
    server.Get(R"(/api/channel.*)", channels);
    server.Post(R"(/api/channel.*)", channels);
    server.Put(R"(/api/channel.*)", channels);
    server.Delete(R"(/api/channel.*)", channels);
}

std::string handle_http_request(std::string_view request, bool draining, std::string_view request_id)
{
    InMemoryHttpServer server;
    auto draining_flag = std::make_shared<std::atomic_bool>(draining);
    server.set_keep_alive_max_count(1);
    server.set_payload_max_length(static_cast<size_t>(config().http_max_body_bytes));
    register_http_routes(server, draining_flag);

    ::httplib::detail::BufferStream stream;
    (void)stream.write(request.data(), request.size());
    const bool ok = server.process(stream, [request_id](::httplib::Request &req) {
        if (!request_id.empty()) {
            req.set_header("X-Request-Id", std::string{ request_id });
        }
        req.remote_addr = "127.0.0.1";
        req.remote_port = 0;
    });

    const std::string &buffer = stream.get_buffer();
    if (!ok || buffer.size() <= request.size()) {
        return serialize_json_http_bytes(400, "Bad Request", json("bad request"), request_id);
    }
    return buffer.substr(request.size());
}

std::string inject_request_metadata(std::string_view request, std::string_view client_ip)
{
    std::string enriched{ request };
    const size_t request_line_end = enriched.find("\r\n");
    if (request_line_end == std::string::npos || client_ip.empty()) {
        return enriched;
    }
    enriched.insert(request_line_end + 2, "X-Revlm-Remote-Ip: " + std::string{ client_ip } + "\r\n");
    enriched.insert(request_line_end + 2, "X-Revlm-Client-Ip: " + std::string{ client_ip } + "\r\n");
    return enriched;
}

} // namespace revlm
