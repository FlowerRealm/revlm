#include "server/http_dispatch.hpp"

#include "auth/security.hpp"
#include "auth/session.hpp"
#include "auth/users.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "config/app_settings.hpp"
#include "models/models.hpp"
#include "proxy_request/gateway.hpp"
#include "proxy_request/responses_proxy.hpp"
#include "proxy_request/token_auth.hpp"
#include "request/request.hpp"
#include "server/tokens.hpp"
#include "store/database.hpp"
#include "util/datetime.hpp"
#include "util/http_query.hpp"
#include "util/json_convert.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"
#include "util/user_input.hpp"
#include "revlm_entities-odb.hxx"

#include <boost/json.hpp>
#include <httplib.h>
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
#include <unordered_set>
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

struct BodyPolicy {
    bool allow = true;
    bool cache = false;
};

struct AdminUserUpdateInput {
    std::optional<std::string> email;
    std::optional<int> status;
    std::optional<std::string> role;
};

struct RequestContext {
    ParsedRequest parsed;
    std::string raw_request;
    std::string request_id;
    std::string client_ip;
};

std::string serialize_response_bytes(const HttpResponse &response)
{
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << ' ' << response.reason << "\r\n"
        << "Content-Type: " << response.content_type << "\r\n"
        << "Content-Length: " << response.body.size() << "\r\n";
    for (const Header &header : response.headers) {
        out << header.name << ": " << header.value << "\r\n";
    }
    out << "Connection: close\r\n"
        << "\r\n";
    std::string bytes = out.str();
    bytes.append(response.body);
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

std::string make_request_id()
{
    return std::to_string(make_usage_event_id());
}

std::string api_success()
{
    boost::json::object body;
    body["success"] = true;
    body["message"] = "";
    return boost::json::serialize(body) + "\n";
}

std::string api_success(boost::json::value data)
{
    boost::json::object body;
    body["success"] = true;
    body["message"] = "";
    body["data"] = std::move(data);
    return boost::json::serialize(body) + "\n";
}

std::string api_failure(std::string_view message)
{
    boost::json::object body;
    body["success"] = false;
    body["message"] = message;
    return boost::json::serialize(body) + "\n";
}

boost::json::object model_item_object(std::string_view id, std::string_view owned_by)
{
    boost::json::object body;
    body["id"] = id;
    body["object"] = "model";
    body["created"] = 0;
    body["owned_by"] = owned_by.empty() ? std::string_view{ "revlm" } : owned_by;
    return body;
}

std::string model_item_json(std::string_view id, std::string_view owned_by)
{
    return boost::json::serialize(model_item_object(id, owned_by));
}

GatewayParsedRequest to_gateway_parsed(const ParsedRequest &parsed)
{
    return GatewayParsedRequest{
        .method = parsed.method,
        .path = parsed.path,
        .target = parsed.target,
        .header_bytes = parsed.header_bytes,
        .content_length = parsed.content_length,
        .invalid_framing = parsed.invalid_framing,
    };
}

boost::json::object admin_settings_json(const AdminSettingsSnapshot &settings)
{
    boost::json::object body;
    body["site_base_url"] = settings.site_base_url;
    body["site_base_url_override"] = settings.site_base_url_override;
    body["site_base_url_effective"] = settings.site_base_url_effective;
    body["site_base_url_invalid"] = settings.site_base_url_invalid;
    body["billing_paygo_price_multiplier"] = settings.billing_paygo_price_multiplier;
    body["billing_paygo_price_multiplier_override"] = settings.billing_paygo_price_multiplier_override;
    return body;
}

HttpResponse api_json_response(std::string body, std::string_view request_id, const std::vector<Header> &headers = {})
{
    return http_response(200, "OK", body, "application/json; charset=utf-8", request_id, headers);
}

std::string plain_token_response(long long token_id, std::string_view token)
{
    boost::json::object data;
    data["token_id"] = token_id;
    data["token"] = token;
    return api_success(std::move(data));
}

std::string mysql_datetime_from_unix(long long unix_seconds)
{
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return buffer;
}

std::string owned_by_for_model_item(const Model &model)
{
    return trim_ascii(model.owned_by).empty() ? "revlm" : trim_ascii(model.owned_by);
}

class RegistrationLock {
public:
    explicit RegistrationLock(odb::database &db)
        : db_(db)
    {
        const std::string got = trim_ascii(
            sql_query_one(db_, "SELECT GET_LOCK(" + sql_quote(db_, "revlm.users.register") + ", 5)").value_or(""));
        if (got != "1") {
            throw std::runtime_error("注册锁等待超时");
        }
        locked_ = true;
    }

    RegistrationLock(const RegistrationLock &) = delete;
    RegistrationLock &operator=(const RegistrationLock &) = delete;

    ~RegistrationLock()
    {
        if (!locked_) {
            return;
        }
        try {
            (void)sql_query_one(db_, "SELECT RELEASE_LOCK(" + sql_quote(db_, "revlm.users.register") + ")");
        } catch (const std::exception &) {
        }
    }

private:
    odb::database &db_;
    bool locked_ = false;
};

HttpResponse register_response(std::string_view raw_request, std::string_view body, const Config &config,
                               std::string_view request_id)
{
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    try {
        const std::string email = normalize_email(json_object_string(*object, "email"));
        const std::string username = normalize_username(json_object_string(*object, "username"));
        const std::string password = json_object_string(*object, "password");
        if (password.empty()) {
            return api_json_response(api_failure("邮箱或密码不能为空"), request_id);
        }
        const std::string password_hash = hash_password(password);

        auto db = make_database(config.db_dsn);
        UserStore store(*db);
        SessionStore sessions(*db);
        RegistrationLock lock(*db);
        const std::string role = store.count_users() == 0 ? "root" : "user";
        User user(email, username, password_hash, role);
        user.status = 1;
        user.id = store.create_user(user);
        const SessionCookie session = make_session_cookie(user.id, session_secret_for_config(config));
        sessions.upsert_session_binding_payload(user.id, session_binding_hash(session.key), "web",
                                                mysql_datetime_from_unix(session.expires_unix));
        return api_json_response(api_success(to_json(user)), request_id,
                                 { Header{ "Set-Cookie", set_session_cookie_header(session.value, raw_request) } });
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("创建用户失败（可能邮箱或账号名已存在）"), request_id);
    }
}

HttpResponse login_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                            std::string_view body)
{
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
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
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        UserStore store(*db);
        SessionStore sessions(*db);
        User user = store.get_user_by_email(lowercase_ascii(login));
        if (user.id == 0) {
            user = store.get_user_by_username(login);
        }
        if (user.id == 0 || user.status != 1 || !check_password(user.password_hash, password)) {
            return api_json_response(api_failure("邮箱/账号名或密码错误"), request_id);
        }
        const SessionCookie session = make_session_cookie(user.id, session_secret_for_config(config));
        sessions.upsert_session_binding_payload(user.id, session_binding_hash(session.key), "web",
                                                mysql_datetime_from_unix(session.expires_unix));
        return api_json_response(api_success(to_json(user)), request_id,
                                 { Header{ "Set-Cookie", set_session_cookie_header(session.value, raw_request) } });
    } catch (const std::exception &) {
        return api_json_response(api_failure("邮箱/账号名或密码错误"), request_id);
    }
}

std::optional<User> authenticated_user(std::string_view raw_request, const Config &config, std::string &failure_message,
                                       bool &clear_cookie, std::string *binding_hash = nullptr)
{
    const WebSessionAuth auth = authenticate_web_session(raw_request, config, binding_hash != nullptr);
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

HttpResponse self_response(std::string_view raw_request, const Config &config, std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_user(raw_request, config, failure, clear_cookie);
    if (!user.has_value()) {
        std::vector<Header> headers;
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return api_json_response(api_failure(failure), request_id, headers);
    }
    return api_json_response(api_success(to_json(*user)), request_id);
}

HttpResponse logout_response(std::string_view raw_request, const Config &config, std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    std::string binding_hash;
    const auto user = authenticated_user(raw_request, config, failure, clear_cookie, &binding_hash);
    if (!user.has_value()) {
        std::vector<Header> headers;
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return api_json_response(api_failure(failure), request_id, headers);
    }
    try {
        auto db = make_database(config.db_dsn);
        SessionStore sessions(*db);
        sessions.delete_session_binding(user->id, binding_hash);
    } catch (const std::exception &) {
        return api_json_response(api_failure("无法清理会话，请重试"), request_id);
    }
    return api_json_response(api_success(), request_id,
                             { Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
}

std::optional<User> api_authenticated_user(std::string_view raw_request, const Config &config,
                                           std::string_view request_id, HttpResponse &response)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_user(raw_request, config, failure, clear_cookie);
    if (user.has_value()) {
        return user;
    }
    std::vector<Header> headers;
    if (clear_cookie) {
        headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
    }
    response = api_json_response(api_failure(failure), request_id, headers);
    return std::nullopt;
}

HttpResponse list_user_tokens_response(std::string_view raw_request, const Config &config, std::string_view request_id)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    try {
        auto db = make_database(config.db_dsn);
        UserStore users(*db);
        TokenStore &store = users.tokens();
        const std::vector<UserToken> tokens = store.list_user_tokens(user->id);
        boost::json::array data;
        for (const UserToken &token : tokens) {
            data.push_back(to_json(token));
        }
        return api_json_response(api_success(std::move(data)), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询 Token 列表失败"), request_id);
    }
}

HttpResponse create_user_token_response(std::string_view raw_request, std::string_view body, const Config &config,
                                        std::string_view request_id)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }

    std::optional<std::string> token_name;
    if (!trim_ascii(body).empty()) {
        const auto object = parse_json_object(body);
        if (!object.has_value()) {
            return api_json_response(api_failure("无效的参数"), request_id);
        }
        std::string name = trim_ascii(json_object_string(*object, "name"));
        if (!name.empty()) {
            token_name = name;
        }
    }

    try {
        const std::string raw_token = new_random_token("sk_", 32);
        auto db = make_database(config.db_dsn);
        UserStore users(*db);
        TokenStore &store = users.tokens();
        odb::nullable<std::string> name;
        if (token_name.has_value()) {
            name = *token_name;
        }
        const long long token_id = store.create_user_token(user->id, name, raw_token);
        return api_json_response(plain_token_response(token_id, raw_token), request_id,
                                 { Header{ "Cache-Control", "no-store" } });
    } catch (const std::exception &) {
        return api_json_response(api_failure("创建令牌失败"), request_id);
    }
}

HttpResponse reveal_user_token_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                        long long token_id)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    if (token_id <= 0) {
        return api_json_response(api_failure("token_id 不合法"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        UserStore users(*db);
        TokenStore &store = users.tokens();
        const auto token = store.reveal_user_token(user->id, token_id);
        if (!token.has_value()) {
            return api_json_response(api_failure("令牌不存在"), request_id);
        }
        return api_json_response(plain_token_response(token_id, *token), request_id,
                                 { Header{ "Cache-Control", "no-store" } });
    } catch (const std::exception &) {
        return api_json_response(api_failure("查看失败"), request_id);
    }
}

HttpResponse rotate_user_token_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                        long long token_id)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    if (token_id <= 0) {
        return api_json_response(api_failure("token_id 不合法"), request_id);
    }
    try {
        const std::string raw_token = new_random_token("sk_", 32);
        auto db = make_database(config.db_dsn);
        UserStore users(*db);
        TokenStore &store = users.tokens();
        if (!store.rotate_user_token(user->id, token_id, raw_token)) {
            return api_json_response(api_failure("令牌不存在"), request_id);
        }
        return api_json_response(plain_token_response(token_id, raw_token), request_id,
                                 { Header{ "Cache-Control", "no-store" } });
    } catch (const std::exception &) {
        return api_json_response(api_failure("重新生成失败"), request_id);
    }
}

HttpResponse revoke_user_token_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                        long long token_id)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    if (token_id <= 0) {
        return api_json_response(api_failure("token_id 不合法"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        UserStore users(*db);
        TokenStore &store = users.tokens();
        store.revoke_user_token(user->id, token_id);
        return api_json_response(api_success(), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("撤销失败"), request_id);
    }
}

HttpResponse delete_user_token_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                        long long token_id)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    if (token_id <= 0) {
        return api_json_response(api_failure("token_id 不合法"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        UserStore users(*db);
        TokenStore &store = users.tokens();
        if (!store.delete_user_token(user->id, token_id)) {
            return api_json_response(api_failure("令牌不存在"), request_id);
        }
        return api_json_response(api_success(), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("删除失败"), request_id);
    }
}

HttpResponse token_channel_groups_response(std::string_view raw_request, const Config &config,
                                           std::string_view request_id, long long token_id)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    if (token_id <= 0) {
        return api_json_response(api_failure("token_id 不合法"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        UserStore users(*db);
        TokenStore &store = users.tokens();
        if (!store.get_user_token_by_id(user->id, token_id).has_value()) {
            return api_json_response(api_failure("令牌不存在"), request_id);
        }

        const std::vector<ChannelGroup> groups = store.list_channel_groups();
        const std::vector<TokenChannelGroupBinding> bindings = store.list_token_channel_group_bindings(token_id);
        const std::vector<TokenChannelGroupBinding> effective =
            store.list_effective_token_channel_group_bindings(token_id);
        std::unordered_set<std::string> bound_group_names;
        bound_group_names.reserve(bindings.size());
        for (const TokenChannelGroupBinding &binding : bindings) {
            const std::string name = trim_ascii(binding.channel_group_name);
            if (!name.empty()) {
                bound_group_names.insert(name);
            }
        }

        boost::json::array allowed_json;
        for (const ChannelGroup &group : groups) {
            const std::string name = trim_ascii(group.name);
            if (name.empty()) {
                continue;
            }
            if (group.status != 1 && !bound_group_names.contains(name)) {
                continue;
            }
            allowed_json.push_back(to_json(group));
        }

        auto bindings_json = [](const std::vector<TokenChannelGroupBinding> &rows) {
            boost::json::array out;
            for (const TokenChannelGroupBinding &row : rows) {
                const std::string name = trim_ascii(row.channel_group_name);
                if (name.empty()) {
                    continue;
                }
                boost::json::object item;
                item["channel_group_name"] = name;
                item["priority"] = row.priority;
                out.push_back(std::move(item));
            }
            return out;
        };

        boost::json::object data;
        data["token_id"] = token_id;
        data["allowed_channel_groups"] = std::move(allowed_json);
        data["bindings"] = bindings_json(bindings);
        data["effective_bindings"] = bindings_json(effective);
        return api_json_response(api_success(std::move(data)), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询 Token 渠道组失败"), request_id);
    }
}

HttpResponse replace_token_channel_groups_response(std::string_view raw_request, const Config &config,
                                                   std::string_view request_id, long long token_id,
                                                   std::string_view body)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    if (token_id <= 0) {
        return api_json_response(api_failure("token_id 不合法"), request_id);
    }

    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    const boost::json::value *groups_field = object->if_contains("channel_groups");
    if (groups_field == nullptr || !groups_field->is_array()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    std::vector<std::string> channel_groups;
    for (const auto &item : groups_field->as_array()) {
        if (!item.is_string()) {
            return api_json_response(api_failure("无效的参数"), request_id);
        }
        channel_groups.emplace_back(item.as_string().data(), item.as_string().size());
    }

    try {
        auto db = make_database(config.db_dsn);
        UserStore users(*db);
        TokenStore &store = users.tokens();
        if (!store.get_user_token_by_id(user->id, token_id).has_value()) {
            return api_json_response(api_failure("令牌不存在"), request_id);
        }
        if (!store.replace_token_channel_groups(token_id, channel_groups)) {
            return api_json_response(api_failure("令牌不存在"), request_id);
        }
        return api_json_response(api_success(), request_id);
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询 Token 失败"), request_id);
    }
}

HttpResponse account_email_response(std::string_view raw_request, std::string_view body, const Config &config,
                                    std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_user(raw_request, config, failure, clear_cookie);
    if (!user.has_value()) {
        std::vector<Header> headers;
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return api_json_response(api_failure(failure), request_id, headers);
    }

    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    const std::string current_password = json_object_string(*object, "current_password");
    if (current_password.empty()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    try {
        const std::string email = normalize_email(json_object_string(*object, "email"));
        auto db = make_database(config.db_dsn);
        UserStore store(*db);
        SessionStore sessions(*db);
        User locked_user = store.get_user_by_id(user->id);
        if (locked_user.id == 0) {
            return api_json_response(api_failure("未登录"), request_id,
                                     { Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
        }
        if (locked_user.status != 1) {
            return api_json_response(api_failure("账号已被禁用"), request_id,
                                     { Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
        }
        if (!check_password(locked_user.password_hash, current_password)) {
            return api_json_response(api_failure("旧密码错误"), request_id);
        }
        locked_user.email = email;
        if (!store.update_user(locked_user)) {
            return api_json_response(api_failure("更新邮箱失败（可能邮箱已存在）"), request_id);
        }
        sessions.delete_all_session_bindings(user->id);
        return api_json_response(api_success(boost::json::object{ { "force_logout", true } }), request_id,
                                 { Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("更新邮箱失败（可能邮箱已存在）"), request_id);
    }
}

HttpResponse account_password_response(std::string_view raw_request, std::string_view body, const Config &config,
                                       std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_user(raw_request, config, failure, clear_cookie);
    if (!user.has_value()) {
        std::vector<Header> headers;
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return api_json_response(api_failure(failure), request_id, headers);
    }

    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    const std::string old_password = json_object_string(*object, "old_password");
    const std::string new_password = json_object_string(*object, "new_password");
    if (old_password.empty() || new_password.empty()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    try {
        const std::string password_hash = hash_password(new_password);
        auto db = make_database(config.db_dsn);
        UserStore store(*db);
        SessionStore sessions(*db);
        User locked_user = store.get_user_by_id(user->id);
        if (locked_user.id == 0) {
            return api_json_response(api_failure("未登录"), request_id,
                                     { Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
        }
        if (locked_user.status != 1) {
            return api_json_response(api_failure("账号已被禁用"), request_id,
                                     { Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
        }
        if (!check_password(locked_user.password_hash, old_password)) {
            return api_json_response(api_failure("旧密码错误"), request_id);
        }
        locked_user.password_hash = password_hash;
        if (!store.update_user(locked_user)) {
            return api_json_response(api_failure("更新密码失败"), request_id);
        }
        sessions.delete_all_session_bindings(user->id);
        return api_json_response(api_success(boost::json::object{ { "force_logout", true } }), request_id,
                                 { Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("更新密码失败"), request_id);
    }
}

std::optional<User> authenticated_admin_user(std::string_view raw_request, const Config &config,
                                             std::string &failure_message, bool &clear_cookie)
{
    const WebSessionAuth auth = authenticate_root_web_session(raw_request, config);
    failure_message = auth.failure_message;
    clear_cookie = auth.clear_cookie;
    if (!auth.ok) {
        return std::nullopt;
    }
    return auth.user;
}

HttpResponse token_models_response(const ::httplib::Request &req, const Config &config, std::string_view request_id)
{
    TokenAuthResult auth_result = authenticated_token(req, config);
    if (!auth_result.auth.has_value()) {
        return http_response(auth_result.status, auth_result.status == 401 ? "Unauthorized" : "Bad Gateway",
                             auth_result.message + "\n", "text/plain; charset=utf-8", request_id);
    }
    const TokenAuth &auth = *auth_result.auth;
    if (auth.groups.empty()) {
        return http_response(400, "Bad Request", "Token 未配置渠道组\n", "text/plain; charset=utf-8", request_id);
    }

    try {
        auto db = make_database(config.db_dsn);
        bool allow_openai = false;
        bool allow_anthropic = false;
        const auto type_rows =
            sql_query_rows(*db, "SELECT DISTINCT c.type FROM channels c "
                                "JOIN channel_group_members cgm ON cgm.channel_id=c.id "
                                "JOIN channel_groups cg ON cg.id=cgm.channel_group_id AND cg.status=1 "
                                "JOIN token_channel_groups tcg ON tcg.channel_group_id=cg.id "
                                "WHERE tcg.token_id=" +
                                    std::to_string(auth.token_id) + " AND c.status=1");
        for (const auto &row : type_rows) {
            const int type = static_cast<int>(std::stoll(row[0].value_or("0")));
            if (type == 1 || type == 2) {
                allow_openai = true;
            }
            if (type == 4) {
                allow_anthropic = true;
            }
        }

        const std::vector<Model> &targets = ModelManager::instance().models();
        std::vector<Model> reachable;
        reachable.reserve(targets.size());
        for (const Model &item : targets) {
            const std::string owned = owned_by_for_model_item(item);
            if (owned == "openai") {
                if (!allow_openai) {
                    continue;
                }
            } else if (owned == "anthropic") {
                if (!allow_anthropic) {
                    continue;
                }
            } else if (!allow_openai && !allow_anthropic) {
                continue;
            }
            reachable.push_back(item);
        }

        std::string data = "[";
        bool first = true;
        auto append_item = [&](const Model &item) {
            const std::string public_id = trim_ascii(item.name);
            if (public_id.empty()) {
                return;
            }
            if (!first) {
                data += ",";
            }
            first = false;
            data += boost::json::serialize(model_item_object(public_id, owned_by_for_model_item(item)));
        };

        for (const Model &item : reachable) {
            append_item(item);
        }
        data += "]";
        boost::json::object body;
        body["object"] = "list";
        boost::system::error_code ec;
        body["data"] = boost::json::parse(data, ec);
        return http_response(200, "OK", boost::json::serialize(body) + "\n", "application/json; charset=utf-8",
                             request_id);
    } catch (const std::exception &) {
        return http_response(502, "Bad Gateway", "查询模型目录失败\n", "text/plain; charset=utf-8", request_id);
    }
}

HttpResponse token_model_retrieve_response(const ::httplib::Request &req, const Config &config,
                                           std::string_view request_id, std::string_view requested_model_id)
{
    TokenAuthResult auth_result = authenticated_token(req, config);
    if (!auth_result.auth.has_value()) {
        return http_response(auth_result.status, auth_result.status == 401 ? "Unauthorized" : "Bad Gateway",
                             auth_result.message + "\n", "text/plain; charset=utf-8", request_id);
    }

    const std::string response_id = trim_ascii(requested_model_id);
    if (response_id.empty()) {
        return http_response(404, "Not Found", "not found\n", "text/plain; charset=utf-8", request_id);
    }

    try {
        const std::vector<Model> &models = ModelManager::instance().models();
        const auto model_it = std::ranges::find(models, response_id, &Model::name);
        if (model_it == models.end()) {
            return http_response(404, "Not Found", "not found\n", "text/plain; charset=utf-8", request_id);
        }
        return http_response(200, "OK", model_item_json(response_id, owned_by_for_model_item(*model_it)) + "\n",
                             "application/json; charset=utf-8", request_id);
    } catch (const std::exception &) {
        return http_response(404, "Not Found", "not found\n", "text/plain; charset=utf-8", request_id);
    }
}

HttpResponse admin_settings_get_response(std::string_view raw_request, const Config &config,
                                         std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_admin_user(raw_request, config, failure, clear_cookie);
    if (!user.has_value()) {
        std::vector<Header> headers;
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return api_json_response(api_failure(failure), request_id, headers);
    }

    try {
        auto db = make_database(config.db_dsn);
        AppSettingsStore store(*db);
        return api_json_response(api_success(admin_settings_json(store.get_admin_settings(raw_request))), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询配置失败"), request_id);
    }
}

HttpResponse admin_settings_put_response(std::string_view raw_request, std::string_view body, const Config &config,
                                         std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_admin_user(raw_request, config, failure, clear_cookie);
    if (!user.has_value()) {
        std::vector<Header> headers;
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return api_json_response(api_failure(failure), request_id, headers);
    }

    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    std::string site_base_url;
    if (const auto *site_it = object->if_contains("site_base_url")) {
        if (site_it->is_string()) {
            site_base_url = std::string{ site_it->as_string().data(), site_it->as_string().size() };
        } else if (!site_it->is_null()) {
            return api_json_response(api_failure("无效的参数"), request_id);
        }
    }

    std::optional<double> billing_paygo_price_multiplier;
    if (const auto *paygo_it = object->if_contains("billing_paygo_price_multiplier")) {
        if (paygo_it->is_null()) {
            billing_paygo_price_multiplier = std::nullopt;
        } else if (paygo_it->is_double() || paygo_it->is_int64() || paygo_it->is_uint64()) {
            const double value = paygo_it->to_number<double>();
            if (!(value > 0.0)) {
                return api_json_response(api_failure("无效的参数"), request_id);
            }
            billing_paygo_price_multiplier = value;
        } else {
            return api_json_response(api_failure("无效的参数"), request_id);
        }
    }

    try {
        auto db = make_database(config.db_dsn);
        AppSettingsStore store(*db);
        store.update_admin_settings(AdminSettingsUpdate{
            .site_base_url = std::move(site_base_url),
            .billing_paygo_price_multiplier = billing_paygo_price_multiplier,
        });
        return api_json_response(api_success(admin_settings_json(store.get_admin_settings(raw_request))), request_id);
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("保存失败"), request_id);
    }
}

std::optional<int> parse_json_int_scalar(std::string_view raw)
{
    const std::string trimmed = trim_ascii(raw);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    size_t pos = 0;
    int value = 0;
    try {
        value = std::stoi(trimmed, &pos, 10);
    } catch (const std::exception &) {
        return std::nullopt;
    }
    if (pos != trimmed.size()) {
        return std::nullopt;
    }
    return value;
}

boost::json::array admin_users_json(std::vector<User> users, odb::database &)
{
    std::sort(users.begin(), users.end(), [](const User &a, const User &b) { return a.id > b.id; });
    boost::json::array data;
    for (const User &user : users) {
        data.push_back(to_json(user));
    }
    return data;
}

std::optional<AdminUserUpdateInput> parse_admin_user_update_body(std::string_view raw_body)
{
    const auto object = parse_json_object(raw_body);
    if (!object.has_value()) {
        return std::nullopt;
    }
    AdminUserUpdateInput input;
    bool has_invalid_status = false;
    for (const auto &field : *object) {
        if (field.key() == "email") {
            if (!field.value().is_string()) {
                continue;
            }
            input.email = std::string{ field.value().as_string().data(), field.value().as_string().size() };
        } else if (field.key() == "role") {
            if (!field.value().is_string()) {
                continue;
            }
            input.role = std::string{ field.value().as_string().data(), field.value().as_string().size() };
        } else if (field.key() == "status") {
            int status = 0;
            if (parse_json_int(field.value(), status)) {
                input.status = status;
            } else if (field.value().is_string()) {
                const auto parsed = parse_json_int_scalar(
                    std::string{ field.value().as_string().data(), field.value().as_string().size() });
                if (!parsed.has_value()) {
                    has_invalid_status = true;
                    break;
                }
                input.status = *parsed;
            } else {
                has_invalid_status = true;
                break;
            }
        }
    }
    if (has_invalid_status) {
        return std::nullopt;
    }
    return input;
}

HttpResponse admin_list_users_response(std::string_view raw_request, const Config &config, std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_admin_user(raw_request, config, failure, clear_cookie);
    if (!user.has_value()) {
        std::vector<Header> headers;
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return api_json_response(api_failure(failure), request_id, headers);
    }
    try {
        auto db = make_database(config.db_dsn);
        UserStore store(*db);
        return api_json_response(api_success(admin_users_json(store.list_users(), *db)), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询失败"), request_id);
    }
}

HttpResponse admin_create_user_response(std::string_view raw_request, std::string_view body, const Config &config,
                                        std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_admin_user(raw_request, config, failure, clear_cookie);
    if (!user.has_value()) {
        std::vector<Header> headers;
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return api_json_response(api_failure(failure), request_id, headers);
    }
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    try {
        const std::string email = normalize_email(json_object_string(*object, "email"));
        const std::string username = normalize_username(json_object_string(*object, "username"));
        const std::string password = json_object_string(*object, "password");
        if (password.empty()) {
            return api_json_response(api_failure("邮箱或密码不能为空"), request_id);
        }
        const std::string role = normalize_user_role(json_object_string(*object, "role"), "user");
        const std::string password_hash = hash_password(password);
        auto db = make_database(config.db_dsn);
        UserStore store(*db);
        if (store.get_user_by_username(username).id != 0) {
            return api_json_response(api_failure("账号名已被占用"), request_id);
        }
        User user(email, username, password_hash, role);
        user.status = 1;
        const long long user_id = store.create_user(std::move(user));
        return api_json_response(api_success(boost::json::object{ { "id", user_id } }), request_id);
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("创建失败（可能邮箱或账号名已存在）"), request_id);
    }
}

HttpResponse admin_update_user_response(long long user_id, std::string_view raw_request, std::string_view body,
                                        const Config &config, std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto actor = authenticated_admin_user(raw_request, config, failure, clear_cookie);
    if (!actor.has_value()) {
        std::vector<Header> headers;
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return api_json_response(api_failure(failure), request_id, headers);
    }
    const auto update = parse_admin_user_update_body(body);
    if (!update.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        UserStore store(*db);
        User target = store.get_user_by_id(user_id);
        if (target.id == 0) {
            return http_response(404, "Not Found", api_failure("Not Found"), "application/json; charset=utf-8",
                                 request_id);
        }
        if (user_id == actor->id) {
            if (update->status.has_value() && *update->status == 0) {
                return api_json_response(api_failure("不能禁用当前登录用户"), request_id);
            }
            if (update->role.has_value()) {
                const std::string role = trim_ascii(*update->role);
                if (!role.empty() && role != "root") {
                    return api_json_response(api_failure("不能修改当前登录用户的 root 角色"), request_id);
                }
            }
        }
        if (update->email.has_value()) {
            target.email = normalize_email(*update->email);
        }
        if (update->status.has_value()) {
            target.status = normalize_user_status(*update->status);
        }
        if (update->role.has_value()) {
            target.role = normalize_user_role(*update->role, target.role);
        }
        if (!store.update_user(target)) {
            return api_json_response(api_failure("保存失败"), request_id);
        }

        return api_json_response(api_success(), request_id);
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("保存失败"), request_id);
    }
}

HttpResponse admin_reset_user_password_response(long long user_id, std::string_view raw_request, std::string_view body,
                                                const Config &config, std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_admin_user(raw_request, config, failure, clear_cookie);
    if (!user.has_value()) {
        std::vector<Header> headers;
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return api_json_response(api_failure(failure), request_id, headers);
    }
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    try {
        const std::string password = json_object_string(*object, "password");
        if (trim_ascii(password).empty()) {
            return api_json_response(api_failure("新密码不能为空"), request_id);
        }
        auto db = make_database(config.db_dsn);
        UserStore store(*db);
        User target = store.get_user_by_id(user_id);
        if (target.id == 0) {
            return api_json_response(api_failure("用户不存在"), request_id);
        }
        target.password_hash = hash_password(password);
        if (!store.update_user(target)) {
            return api_json_response(api_failure("保存失败"), request_id);
        }
        return api_json_response(api_success(), request_id);
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("保存失败"), request_id);
    }
}

HttpResponse admin_add_user_balance_response(long long user_id, std::string_view raw_request, std::string_view body,
                                             const Config &config, std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto actor = authenticated_admin_user(raw_request, config, failure, clear_cookie);
    if (!actor.has_value()) {
        std::vector<Header> headers;
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return api_json_response(api_failure(failure), request_id, headers);
    }
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        UserStore store(*db);
        User target = store.get_user_by_id(user_id);
        if (target.id == 0) {
            return api_json_response(api_failure("用户不存在"), request_id);
        }
        const std::string amount_raw = normalize_usd_amount(json_object_string(*object, "amount_usd"));
        target.balance_usd += std::stod(amount_raw);
        if (!store.update_user(target)) {
            return api_json_response(api_failure("用户不存在"), request_id);
        }
        const double balance = UserStore(*db).get_user_balance_usd(user_id);
        return api_json_response(api_success(boost::json::object{ { "balance_usd", balance } }), request_id);
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(std::string{ "入账失败：" } + err.what()), request_id);
    }
}

HttpResponse admin_delete_user_response(long long user_id, std::string_view raw_request, const Config &config,
                                        std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto actor = authenticated_admin_user(raw_request, config, failure, clear_cookie);
    if (!actor.has_value()) {
        std::vector<Header> headers;
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return api_json_response(api_failure(failure), request_id, headers);
    }
    if (user_id == actor->id) {
        return api_json_response(api_failure("不能删除当前登录用户"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        UserStore store(*db);
        if (!store.delete_user(user_id)) {
            return api_json_response(api_failure("用户不存在"), request_id);
        }
        return api_json_response(api_success(), request_id);
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("删除失败"), request_id);
    }
}

HttpResponse billing_balance_response(std::string_view raw_request, const Config &config, std::string_view request_id)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_user(raw_request, config, failure, clear_cookie);
    if (!user.has_value()) {
        std::vector<Header> headers;
        if (clear_cookie) {
            headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
        }
        return api_json_response(api_failure(failure), request_id, headers);
    }
    try {
        auto db = make_database(config.db_dsn);
        UserStore store(*db);
        return api_json_response(
            api_success(boost::json::object{ { "balance_usd", store.get_user_balance_usd(user->id) } }), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
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

std::optional<HttpResponse> validate_parsed_request(const ParsedRequest &parsed, const Config &config,
                                                    std::string_view request_id)
{
    if (parsed.header_bytes > static_cast<size_t>(config.http_max_header_bytes)) {
        return http_response(431, "Request Header Fields Too Large", "request header too large\n",
                             "text/plain; charset=utf-8", request_id);
    }
    if (parsed.content_length > static_cast<size_t>(config.http_max_body_bytes)) {
        return http_response(413, "Payload Too Large", "payload too large\n", "text/plain; charset=utf-8", request_id);
    }
    return std::nullopt;
}

RequestContext make_request_context(const ::httplib::Request &req)
{
    std::string request_id;
    const std::string client_header = req.get_header_value("X-Request-Id");
    if (!client_header.empty()) {
        try {
            std::size_t idx = 0;
            const long long parsed = std::stoll(client_header, &idx);
            if (idx == client_header.size() && parsed > 0) {
                request_id = client_header;
            }
        } catch (...) {
        }
    }
    if (request_id.empty()) {
        request_id = make_request_id();
    }
    const std::string client_ip = req.remote_addr.empty() ? "127.0.0.1" : req.remote_addr;
    return RequestContext{
        .parsed = parsed_request_from_httplib(req),
        .raw_request = inject_request_metadata(build_raw_http_request(req), client_ip),
        .request_id = std::move(request_id),
        .client_ip = client_ip,
    };
}

void log_access(const RequestContext &ctx, int status)
{
    std::cerr << "access request_id=" << ctx.request_id << " status=" << status << " method=" << ctx.parsed.method
              << " path=" << redact_request_target(ctx.parsed.target) << '\n';
}

::httplib::Server::Handler make_http_handler(
    const Config &config,
    std::function<void(const ::httplib::Request &, ::httplib::Response &, const RequestContext &)> handler)
{
    return [&config, handler = std::move(handler)](const ::httplib::Request &req, ::httplib::Response &res) {
        const RequestContext ctx = make_request_context(req);
        if (const std::optional<HttpResponse> validation_error =
                validate_parsed_request(ctx.parsed, config, ctx.request_id);
            validation_error.has_value()) {
            apply_http_response(*validation_error, res);
            log_access(ctx, res.status);
            return;
        }
        handler(req, res, ctx);
        log_access(ctx, res.status);
    };
}

::httplib::Server::Handler
make_response_handler(const Config &config,
                      std::function<HttpResponse(const ::httplib::Request &, const RequestContext &)> handler)
{
    return make_http_handler(config, [handler = std::move(handler)](
                                         const ::httplib::Request &req, ::httplib::Response &res,
                                         const RequestContext &ctx) { apply_http_response(handler(req, ctx), res); });
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

HttpResponse not_found_response(std::string_view request_id)
{
    return http_response(404, "Not Found", "not found\n", "text/plain; charset=utf-8", request_id);
}

class InMemoryHttpServer final : public ::httplib::Server {
public:
    bool process(::httplib::Stream &stream, const std::function<void(::httplib::Request &)> &setup_request)
    {
        bool connection_closed = false;
        return process_request(stream, "127.0.0.1", 0, "127.0.0.1", 0, true, connection_closed, setup_request);
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

std::string mysql_to_iso_utc(std::string_view value)
{
    if (value.empty()) {
        return {};
    }
    std::string out{ value };
    if (out.size() >= 19 && out[10] == ' ') {
        out[10] = 'T';
    }
    if (!out.empty() && out.back() != 'Z') {
        out.push_back('Z');
    }
    return out;
}

std::string status_json_label(std::string_view status)
{
    if (status == "committed" || status == "1" || status == "true" || status == "TRUE") {
        return "committed";
    }
    return std::string{ status };
}

std::string model_icon_url(std::string_view owned_by)
{
    const std::string owner = lowercase_ascii(trim_ascii(owned_by));
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

RequestListFilter filter_from_usage_options(long long user_id, const UsageQueryOptions &options,
                                            bool committed_only = false)
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
    if (committed_only) {
        filter.status = "committed";
    }
    return filter;
}

boost::json::object request_to_user_event_json(const Request &req)
{
    boost::json::object o = to_json(req);
    o["time"] = mysql_to_iso_utc(req.time);
    o["request_id"] = std::to_string(req.id);
    o["channel_id"] = req.channel_id > 0 ? boost::json::value(req.channel_id) : boost::json::value(nullptr);
    o["status"] = status_json_label(req.status);
    const std::string model = !req.model.name.empty() ? req.model.name :
                                                        (req.model_name.null() ? std::string{} : *req.model_name);
    o["model"] = model.empty() ? boost::json::value(nullptr) : boost::json::value(model);
    o["cache_creation_tokens"] = req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
    o["committed_usd"] = request_detail::decimal_to_string(req.solve_price());
    return o;
}

boost::json::object aggregate_window(const std::vector<Request> &rows, const UsageQueryOptions &options,
                                     double balance_usd)
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
    double committed = 0.0;
    std::optional<sys_seconds> min_time;
    std::optional<sys_seconds> max_time;

    for (const Request &req : rows) {
        if (req.status != "committed") {
            continue;
        }
        ++requests;
        input_tokens += req.input_tokens;
        output_tokens += req.output_tokens;
        cache_read_tokens += req.cache_read_tokens;
        cache_creation_tokens += req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
        committed += req.solve_price();
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

    boost::json::object window;
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
    const std::string committed_usd = request_detail::decimal_to_string(committed);
    window["used_usd"] = committed_usd;
    window["committed_usd"] = committed_usd;
    window["limit_usd"] = request_detail::decimal_to_string(balance_usd);
    window["remaining_usd"] = request_detail::decimal_to_string(balance_usd - committed);
    return window;
}

boost::json::array usage_time_series(const std::vector<Request> &rows, const std::string &tz,
                                     std::string_view granularity)
{
    struct Acc {
        long long requests = 0;
        long long tokens = 0;
        double committed_usd = 0.0;
        long long input = 0;
        long long cached = 0;
        long long first_token_total = 0;
        long long first_token_count = 0;
        long long output = 0;
        long long latency_total = 0;
    };
    std::map<std::string, Acc> buckets;
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
        Acc &acc = buckets[bucket];
        ++acc.requests;
        const long long cache_creation = req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
        acc.tokens += req.input_tokens + req.output_tokens + req.cache_read_tokens + cache_creation;
        acc.input += req.input_tokens;
        acc.cached += req.cache_read_tokens + cache_creation;
        if (req.status == "committed") {
            acc.committed_usd += req.solve_price();
        }
        if (req.first_token_latency_ms > 0) {
            acc.first_token_total += req.first_token_latency_ms;
            ++acc.first_token_count;
        }
        if (req.latency_ms > 0) {
            acc.output += req.output_tokens;
            acc.latency_total += req.latency_ms;
        }
    }

    boost::json::array points;
    for (const auto &[bucket, acc] : buckets) {
        boost::json::object point;
        point["bucket"] = bucket;
        point["requests"] = acc.requests;
        point["tokens"] = acc.tokens;
        point["committed_usd"] = acc.committed_usd;
        point["cache_ratio"] = acc.input > 0 ? static_cast<double>(acc.cached) / static_cast<double>(acc.input) : 0.0;
        point["avg_first_token_latency"] = acc.first_token_count > 0 ? static_cast<double>(acc.first_token_total) /
                                                                           static_cast<double>(acc.first_token_count) :
                                                                       0.0;
        point["tokens_per_second"] =
            acc.latency_total > 0 ? static_cast<double>(acc.output) * 1000.0 / static_cast<double>(acc.latency_total) :
                                    0.0;
        points.push_back(std::move(point));
    }
    return points;
}

boost::json::array dashboard_model_stats(const std::vector<Request> &rows)
{
    struct Acc {
        long long requests = 0;
        long long tokens = 0;
        double committed = 0.0;
    };
    std::map<std::string, Acc> by_model;
    for (const Request &req : rows) {
        const std::string model = req.model.name;
        Acc &acc = by_model[model];
        ++acc.requests;
        acc.tokens += req.input_tokens + req.output_tokens + req.cache_read_tokens + req.cache_creation_5m_tokens +
                      req.cache_creation_1h_tokens;
        if (req.status == "committed") {
            acc.committed += req.solve_price();
        }
    }
    std::vector<std::pair<std::string, Acc>> ranked(by_model.begin(), by_model.end());
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
    boost::json::array out;
    for (size_t i = 0; i < ranked.size(); ++i) {
        boost::json::object o;
        o["model"] = ranked[i].first;
        const std::vector<Model> &models = ModelManager::instance().models();
        const auto it =
            std::find_if(models.begin(), models.end(), [&](const Model &m) { return m.name == ranked[i].first; });
        const std::string icon = it != models.end() ? model_icon_url(it->owned_by) : "";
        if (icon.empty()) {
            o["icon_url"] = nullptr;
        } else {
            o["icon_url"] = icon;
        }
        o["color"] = kColors[i % (sizeof(kColors) / sizeof(kColors[0]))];
        o["requests"] = ranked[i].second.requests;
        o["tokens"] = ranked[i].second.tokens;
        o["committed_usd"] = request_detail::decimal_to_string(ranked[i].second.committed);
        out.push_back(std::move(o));
    }
    return out;
}

HttpResponse user_models_detail_http_response(std::string_view raw_request, const Config &config,
                                              std::string_view request_id)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    boost::json::array models_json;
    for (const Model &model : ModelManager::instance().models()) {
        boost::json::object o;
        o["id"] = model.id;
        o["public_id"] = model.name;
        o["group_name"] = "";
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
    return api_json_response(api_success(std::move(models_json)), request_id);
}

HttpResponse dashboard_http_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                     std::string_view target)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return api_json_response(api_failure(message), request_id);
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
        auto db = make_database(config.db_dsn);
        RequestStore store(*db);
        UserStore users(*db);
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        const boost::json::object today = aggregate_window(rows, options, users.get_user_balance_usd(user->id));
        boost::json::object charts;
        charts["model_stats"] = dashboard_model_stats(rows);
        charts["time_series_stats"] = usage_time_series(rows, options.time_zone, "hour");
        boost::json::object body;
        body["today_usage_usd"] = today.at("committed_usd");
        body["today_since"] = today.at("since");
        body["today_until"] = today.at("until");
        body["today_requests"] = today.at("requests");
        body["today_tokens"] = today.at("tokens");
        body["today_rpm"] = std::to_string(today.at("rpm").to_number<long long>());
        body["today_tpm"] = std::to_string(today.at("tpm").to_number<long long>());
        body["charts"] = std::move(charts);
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse usage_windows_http_response(std::string_view raw_request, const Config &config,
                                         std::string_view request_id, std::string_view target)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return api_json_response(api_failure(message), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        RequestStore store(*db);
        UserStore users(*db);
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        boost::json::object body;
        body["time_zone"] = options.time_zone;
        body["now"] = to_iso8601z(date::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        boost::json::array windows;
        windows.push_back(aggregate_window(rows, options, users.get_user_balance_usd(user->id)));
        body["windows"] = std::move(windows);
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse requests_http_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                    std::string_view target)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return api_json_response(api_failure(message), request_id);
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
        auto db = make_database(config.db_dsn);
        RequestStore store(*db);
        auto loaded = store.query(filter);
        const bool has_extra = static_cast<int>(loaded.size()) > limit;
        if (has_extra) {
            loaded.resize(static_cast<size_t>(limit));
        }
        boost::json::object body;
        boost::json::array events;
        for (const Request &req : loaded) {
            events.push_back(request_to_user_event_json(req));
        }
        body["events"] = std::move(events);
        if (has_extra && !loaded.empty()) {
            body["next_before_id"] = loaded.back().id;
        } else {
            body["next_before_id"] = nullptr;
        }
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse usage_timeseries_http_response(std::string_view raw_request, const Config &config,
                                            std::string_view request_id, std::string_view target)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return api_json_response(api_failure(message), request_id);
    }
    std::string granularity = trim_ascii(query_param_value(params, "granularity"));
    if (granularity.empty()) {
        granularity = "day";
    }
    if (granularity != "hour" && granularity != "day") {
        return api_json_response(api_failure("granularity 无效"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        RequestStore store(*db);
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        boost::json::object body;
        body["time_zone"] = options.time_zone;
        body["start"] = options.start_utc.has_value() ? to_iso8601z(*options.start_utc) : "";
        body["end"] = options.end_exclusive_utc.has_value() ?
                          to_iso8601z(*options.end_exclusive_utc - std::chrono::seconds{ 1 }) :
                          "";
        body["granularity"] = granularity;
        body["points"] = usage_time_series(rows, options.time_zone, granularity);
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse usage_event_detail_http_response(std::string_view raw_request, const Config &config,
                                              std::string_view request_id, long long event_id)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    if (event_id <= 0) {
        return api_json_response(api_failure("event_id 无效"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        RequestStore store(*db);
        const auto req = store.get_by_id(event_id);
        if (!req.has_value() || req->user_id != user->id) {
            return api_json_response(api_failure("事件不存在"), request_id);
        }
        boost::json::object body;
        body["event_id"] = req->id;
        body["pricing_breakdown"] = to_json(compute_pricing_breakdown(*req));
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

std::optional<User> api_authenticated_admin(std::string_view raw_request, const Config &config,
                                            std::string_view request_id, HttpResponse &response)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_admin_user(raw_request, config, failure, clear_cookie);
    if (user.has_value()) {
        return user;
    }
    std::vector<Header> headers;
    if (clear_cookie) {
        headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
    }
    response = api_json_response(api_failure(failure.empty() ? "未登录" : failure), request_id, headers);
    return std::nullopt;
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

std::optional<AdminUsageRange> resolve_admin_usage_range(odb::database &db,
                                                         const std::map<std::string, std::string> &params,
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
        RequestStore store(db);
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
                                     int limit, std::string &error, odb::database &db)
{
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

std::string state_label(std::string_view status)
{
    if (status == "pending") {
        return "处理中";
    }
    if (status == "committed") {
        return "已结算";
    }
    if (status == "void") {
        return "已作废";
    }
    if (status == "expired") {
        return "已过期";
    }
    return std::string{ status };
}

std::string state_badge_class(std::string_view status)
{
    if (status == "pending") {
        return "bg-warning-subtle text-warning border border-warning-subtle";
    }
    if (status == "committed") {
        return "bg-success-subtle text-success border border-success-subtle";
    }
    return "bg-secondary-subtle text-secondary border border-secondary-subtle";
}

boost::json::object request_to_admin_event_json(const Request &req, std::string_view user_email,
                                                std::string_view channel_name)
{
    const long long cached_tokens = req.cache_read_tokens + req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
    const std::string status = status_json_label(req.status);
    boost::json::object o = to_json(req);
    o["time"] = mysql_to_iso_utc(req.time);
    o["user_email"] = user_email;
    const std::string model = !req.model.name.empty() ? req.model.name :
                                                        (req.model_name.null() ? std::string{} : *req.model_name);
    o["model"] = model;
    if (req.output_tokens > 0 && req.latency_ms > 0) {
        o["tokens_per_second"] = request_detail::decimal_to_string(static_cast<double>(req.output_tokens) * 1000.0 /
                                                                   static_cast<double>(req.latency_ms));
    } else {
        o["tokens_per_second"] = "-";
    }
    o["cached_tokens"] = cached_tokens;
    const std::string committed = request_detail::decimal_to_string(req.solve_price());
    o["cost_usd"] = committed;
    o["committed_usd"] = committed;
    o["status"] = status;
    o["state_label"] = state_label(status);
    o["state_badge_class"] = state_badge_class(status);
    o["upstream_channel_name"] = channel_name;
    o["request_id"] = std::to_string(req.id);
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

boost::json::object admin_window_summary(const AdminUsageRange &range, const std::vector<Request> &rows,
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
    double committed = 0.0;
    for (const Request &req : rows) {
        if (req.status != "committed") {
            continue;
        }
        ++requests;
        input_tokens += req.input_tokens;
        output_tokens += req.output_tokens;
        cache_read_tokens += req.cache_read_tokens;
        cache_creation_tokens += req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
        committed += req.solve_price();
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
        if (req.status != "committed") {
            continue;
        }
        ++recent_requests;
        recent_tokens += req.input_tokens + req.output_tokens;
    }
    const double total_tokens = static_cast<double>(input_tokens + output_tokens);
    const double cached_tokens = static_cast<double>(cache_read_tokens + cache_creation_tokens);
    boost::json::object o;
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
    o["committed_usd"] = request_detail::decimal_to_string(committed);
    return o;
}

boost::json::array top_users_json(odb::database &db, const std::vector<Request> &rows)
{
    struct Acc {
        std::string email;
        std::string role;
        long long status = 0;
        double committed = 0.0;
    };
    std::map<long long, Acc> by_user;
    UserStore users(db);
    for (const Request &req : rows) {
        if (req.status != "committed") {
            continue;
        }
        Acc &acc = by_user[req.user_id];
        if (acc.email.empty()) {
            const User u = users.get_user_by_id(req.user_id);
            acc.email = u.email;
            acc.role = u.role;
            acc.status = u.status;
        }
        acc.committed += req.solve_price();
    }
    std::vector<std::pair<long long, Acc>> ranked(by_user.begin(), by_user.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        if (a.second.committed != b.second.committed) {
            return a.second.committed > b.second.committed;
        }
        return a.first > b.first;
    });
    if (ranked.size() > 50) {
        ranked.resize(50);
    }
    boost::json::array out;
    for (const auto &entry : ranked) {
        boost::json::object o;
        o["user_id"] = entry.first;
        o["email"] = entry.second.email;
        o["role"] = entry.second.role;
        o["status"] = entry.second.status;
        o["committed_usd"] = request_detail::decimal_to_string(entry.second.committed);
        out.push_back(std::move(o));
    }
    return out;
}

HttpResponse admin_dashboard_http_response(std::string_view raw_request, const Config &config,
                                           std::string_view request_id)
{
    HttpResponse auth_response;
    if (!api_authenticated_admin(raw_request, config, request_id, auth_response)) {
        return auth_response;
    }
    try {
        auto db = make_database(config.db_dsn);
        UserStore users(*db);
        ChannelStore channels(*db);
        const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto local = date::make_zoned(std::string{ kAdminTimeZone }, now_utc).get_local_time();
        const date::year_month_day ymd{ date::floor<date::days>(local) };
        const sys_seconds today_start =
            local_date_to_utc(static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                              static_cast<unsigned>(ymd.day()), std::string{ kAdminTimeZone });

        RequestListFilter filter;
        filter.start = to_mysql_datetime(today_start);
        filter.end_exclusive = to_mysql_datetime(now_utc);
        RequestStore store(*db);
        const auto rows = store.query(filter);
        long long requests_today = 0;
        long long input_tokens = 0;
        long long output_tokens = 0;
        double cost = 0.0;
        for (const Request &req : rows) {
            if (req.status != "committed") {
                continue;
            }
            ++requests_today;
            input_tokens += req.input_tokens;
            output_tokens += req.output_tokens;
            cost += req.solve_price();
        }
        boost::json::object stats;
        stats["users_count"] = users.count_users();
        const auto channel_list = channels.list_channels();
        stats["channels_count"] = static_cast<long long>(channel_list.size());
        stats["endpoints_count"] = static_cast<long long>(channel_list.size());
        stats["requests_today"] = requests_today;
        stats["tokens_today"] = input_tokens + output_tokens;
        stats["input_tokens_today"] = input_tokens;
        stats["output_tokens_today"] = output_tokens;
        stats["cost_today"] = request_detail::decimal_to_string(cost);
        boost::json::object data;
        data["admin_time_zone"] = kAdminTimeZone;
        data["stats"] = std::move(stats);
        return api_json_response(api_success(std::move(data)), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("读取统计失败"), request_id);
    }
}

HttpResponse admin_usage_page_http_response(std::string_view raw_request, const Config &config,
                                            std::string_view request_id, std::string_view target)
{
    HttpResponse auth_response;
    if (!api_authenticated_admin(raw_request, config, request_id, auth_response)) {
        return auth_response;
    }
    const auto params = parse_query_map(target);
    int limit = 50;
    const std::string limit_raw = query_param_value(params, "limit");
    if (!limit_raw.empty() && !parse_i32(limit_raw, limit)) {
        return api_json_response(api_failure("limit 不合法"), request_id);
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
        return api_json_response(api_failure("summary 不合法"), request_id);
    }

    try {
        auto db = make_database(config.db_dsn);
        const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        std::string range_error;
        const auto range = resolve_admin_usage_range(*db, params, now_utc, range_error);
        if (!range.has_value()) {
            return api_json_response(api_failure(range_error), request_id);
        }
        std::string filter_error;
        RequestListFilter page_filter = build_admin_filter(params, *range, limit + 1, filter_error, *db);
        if (!filter_error.empty()) {
            return api_json_response(api_failure(filter_error), request_id);
        }
        RequestStore store(*db);
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
        UserStore users(*db);
        ChannelStore channels(*db);
        for (const Channel &c : channels.list_channels()) {
            channel_names[c.id] = c.name;
        }

        boost::json::array events;
        for (const Request &req : loaded) {
            if (!emails.contains(req.user_id)) {
                emails[req.user_id] = users.get_user_by_id(req.user_id).email;
            }
            events.push_back(request_to_admin_event_json(
                req, emails[req.user_id], channel_names.contains(req.channel_id) ? channel_names[req.channel_id] : ""));
        }

        boost::json::object data;
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
            data["top_users"] = top_users_json(*db, summary_rows);
        }
        return api_json_response(api_success(std::move(data)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse admin_usage_event_detail_http_response(std::string_view raw_request, const Config &config,
                                                    std::string_view request_id, long long event_id)
{
    HttpResponse auth_response;
    if (!api_authenticated_admin(raw_request, config, request_id, auth_response)) {
        return auth_response;
    }
    if (event_id <= 0) {
        return api_json_response(api_failure("event_id 不合法"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        RequestStore store(*db);
        const auto req = store.get_by_id(event_id);
        if (!req.has_value()) {
            return api_json_response(api_failure("not found"), request_id);
        }
        boost::json::object body;
        body["event_id"] = req->id;
        body["pricing_breakdown"] = to_json(compute_pricing_breakdown(*req));
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询失败"), request_id);
    }
}

HttpResponse admin_usage_timeseries_http_response(std::string_view raw_request, const Config &config,
                                                  std::string_view request_id, std::string_view target)
{
    HttpResponse auth_response;
    if (!api_authenticated_admin(raw_request, config, request_id, auth_response)) {
        return auth_response;
    }
    std::map<std::string, std::string> params = parse_query_map(target);
    std::string granularity = lowercase_ascii(trim_ascii(query_param_value(params, "granularity")));
    if (granularity.empty()) {
        granularity = "hour";
    }
    if (granularity != "hour" && granularity != "day") {
        return api_json_response(api_failure("granularity 仅支持 hour/day"), request_id);
    }
    bool all_time = false;
    const std::string all_time_raw = query_param_value(params, "all_time");
    if (!all_time_raw.empty() && !parse_bool_flag(all_time_raw, all_time)) {
        return api_json_response(api_failure("all_time 不合法"), request_id);
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
        auto db = make_database(config.db_dsn);
        std::string range_error;
        const auto range = resolve_admin_usage_range(*db, params, now_utc, range_error);
        if (!range.has_value()) {
            return api_json_response(api_failure(range_error), request_id);
        }
        std::string filter_error;
        RequestListFilter filters = build_admin_filter(params, *range, 0, filter_error, *db);
        if (!filter_error.empty()) {
            return api_json_response(api_failure(filter_error), request_id);
        }
        RequestStore store(*db);
        const auto rows = store.query(filters);
        boost::json::object body;
        body["admin_time_zone"] = kAdminTimeZone;
        body["start"] = range->start;
        body["end"] = range->end;
        body["granularity"] = granularity;
        body["points"] = usage_time_series(rows, std::string{ kAdminTimeZone }, granularity);
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询失败"), request_id);
    }
}

} // namespace

void register_http_routes(::httplib::Server &server, const Config &config,
                          const std::shared_ptr<std::atomic_bool> &draining)
{
    auto api = [&config](auto fn) {
        return make_response_handler(
            config, [fn = std::move(fn)](const ::httplib::Request &req, const RequestContext &ctx) -> HttpResponse {
                return fn(req, ctx);
            });
    };
    auto any = [&config](auto fn) {
        return make_response_handler(
            config, [fn = std::move(fn)](const ::httplib::Request &req, const RequestContext &ctx) -> HttpResponse {
                return fn(req, ctx);
            });
    };
    auto api_stream = [&config](auto fn) {
        return make_http_handler(config, [fn = std::move(fn)](const ::httplib::Request &req, ::httplib::Response &res,
                                                              const RequestContext &ctx) { fn(req, res, ctx); });
    };

    server.Get("/healthz", any([](const ::httplib::Request &, const RequestContext &ctx) {
                   return http_response(200, "OK", "ok\n", "text/plain; charset=utf-8", ctx.request_id);
               }));
    server.Get("/livez", any([](const ::httplib::Request &, const RequestContext &ctx) {
                   return http_response(200, "OK", "ok\n", "text/plain; charset=utf-8", ctx.request_id);
               }));
    server.Get("/readyz", any([draining](const ::httplib::Request &, const RequestContext &ctx) {
                   if (draining->load()) {
                       return http_response(503, "Service Unavailable", "draining\n", "text/plain; charset=utf-8",
                                            ctx.request_id);
                   }
                   return http_response(200, "OK", "ok\n", "text/plain; charset=utf-8", ctx.request_id);
               }));
    server.Get("/metrics", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return http_response(200, "OK", "", "text/plain; version=0.0.4; charset=utf-8", ctx.request_id);
               }));
    server.Get("/api/user/self", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return self_response(ctx.raw_request, config, ctx.request_id);
               }));
    server.Get("/api/user/logout", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return logout_response(ctx.raw_request, config, ctx.request_id);
               }));
    server.Get("/api/user/models/detail", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return user_models_detail_http_response(ctx.raw_request, config, ctx.request_id);
               }));
    server.Get("/api/dashboard", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return dashboard_http_response(ctx.raw_request, config, ctx.request_id, ctx.parsed.target);
               }));
    server.Get("/api/request/windows", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return usage_windows_http_response(ctx.raw_request, config, ctx.request_id, ctx.parsed.target);
               }));
    server.Get("/api/request/events", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return requests_http_response(ctx.raw_request, config, ctx.request_id, ctx.parsed.target);
               }));
    server.Get("/api/request/timeseries", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return usage_timeseries_http_response(ctx.raw_request, config, ctx.request_id, ctx.parsed.target);
               }));
    server.Get("/api/request/events/:event_id/detail",
               api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                   const auto event_id = path_param_i64(req, "event_id");
                   if (!event_id.has_value()) {
                       return api_json_response(api_failure("event_id 无效"), ctx.request_id);
                   }
                   return usage_event_detail_http_response(ctx.raw_request, config, ctx.request_id, *event_id);
               }));
    server.Get("/api/token", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return list_user_tokens_response(ctx.raw_request, config, ctx.request_id);
               }));
    server.Post("/api/token", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                    return create_user_token_response(ctx.raw_request, req.body, config, ctx.request_id);
                }));
    server.Get("/api/token/:token_id/reveal", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ?
                              reveal_user_token_response(ctx.raw_request, config, ctx.request_id, *token_id) :
                              api_json_response(api_failure("token_id 不合法"), ctx.request_id);
               }));
    server.Post("/api/token/:token_id/rotate", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                    const auto token_id = path_param_i64(req, "token_id");
                    return token_id.has_value() ?
                               rotate_user_token_response(ctx.raw_request, config, ctx.request_id, *token_id) :
                               api_json_response(api_failure("token_id 不合法"), ctx.request_id);
                }));
    server.Post("/api/token/:token_id/revoke", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                    const auto token_id = path_param_i64(req, "token_id");
                    return token_id.has_value() ?
                               revoke_user_token_response(ctx.raw_request, config, ctx.request_id, *token_id) :
                               api_json_response(api_failure("token_id 不合法"), ctx.request_id);
                }));
    server.Delete("/api/token/:token_id", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                      const auto token_id = path_param_i64(req, "token_id");
                      return token_id.has_value() ?
                                 delete_user_token_response(ctx.raw_request, config, ctx.request_id, *token_id) :
                                 api_json_response(api_failure("token_id 不合法"), ctx.request_id);
                  }));
    server.Get("/api/token/:token_id/channel-groups",
               api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ?
                              token_channel_groups_response(ctx.raw_request, config, ctx.request_id, *token_id) :
                              api_json_response(api_failure("token_id 不合法"), ctx.request_id);
               }));
    server.Put("/api/token/:token_id/channel-groups",
               api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ?
                              replace_token_channel_groups_response(ctx.raw_request, config, ctx.request_id, *token_id,
                                                                    req.body) :
                              api_json_response(api_failure("token_id 不合法"), ctx.request_id);
               }));
    server.Post("/api/user/register", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                    return register_response(ctx.raw_request, req.body, config, ctx.request_id);
                }));
    server.Post("/api/user/login", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                    return login_response(ctx.raw_request, config, ctx.request_id, req.body);
                }));
    server.Post("/api/account/email", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                    return account_email_response(ctx.raw_request, req.body, config, ctx.request_id);
                }));
    server.Post("/api/account/password", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                    return account_password_response(ctx.raw_request, req.body, config, ctx.request_id);
                }));
    server.Get("/v1/models", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                   return token_models_response(req, config, ctx.request_id);
               }));
    server.Get("/v1beta/openai/models", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                   return token_models_response(req, config, ctx.request_id);
               }));
    server.Get("/v1/models/:model_id", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                   const std::string model_id = path_param_string(req, "model_id");
                   return model_id.empty() ? not_found_response(ctx.request_id) :
                                             token_model_retrieve_response(req, config, ctx.request_id, model_id);
               }));
    server.Post("/v1/chat/completions",
                api_stream([&](const ::httplib::Request &req, ::httplib::Response &res, const RequestContext &ctx) {
                    const GatewayParsedRequest parsed = to_gateway_parsed(ctx.parsed);
                    if (::revlm::parse_json_bool_field(req.body, "stream").value_or(false)) {
                        run_chat_completions_stream(res, req, parsed, config, ctx.request_id, ctx.client_ip);
                        return;
                    }
                    apply_http_response(run_chat_completions_gateway(req, config, ctx.request_id), res);
                }));
    server.Post("/v1/messages",
                api_stream([&](const ::httplib::Request &req, ::httplib::Response &res, const RequestContext &ctx) {
                    const GatewayParsedRequest parsed = to_gateway_parsed(ctx.parsed);
                    if (::revlm::parse_json_bool_field(req.body, "stream").value_or(false)) {
                        run_messages_stream(res, req, parsed, config, ctx.request_id, ctx.client_ip);
                        return;
                    }
                    apply_http_response(run_messages_gateway(req, config, ctx.request_id), res);
                }));
    server.Post("/v1/responses",
                api_stream([&](const ::httplib::Request &req, ::httplib::Response &res, const RequestContext &ctx) {
                    const auto result = handle_responses_proxy_request(
                        ctx.raw_request, ctx.parsed.method, ctx.parsed.path, config, ctx.request_id,
                        ::revlm::parse_json_bool_field(req.body, "stream").value_or(false) ?
                            ResponsesProxyExecuteOptions{ .write_client = {}, .stream_response = &res } :
                            ResponsesProxyExecuteOptions{});
                    if (!result.handled_stream) {
                        apply_http_response(result.response, res);
                    }
                }));
    server.Post("/v1/responses/input_tokens", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                    return handle_responses_proxy_request(ctx.raw_request, ctx.parsed.method, ctx.parsed.path, config,
                                                          ctx.request_id)
                        .response;
                }));
    server.Post("/v1/responses/compact",
                api_stream([&](const ::httplib::Request &req, ::httplib::Response &res, const RequestContext &ctx) {
                    const GatewayParsedRequest parsed = to_gateway_parsed(ctx.parsed);
                    if (::revlm::parse_json_bool_field(req.body, "stream").value_or(false)) {
                        run_responses_compact_stream(res, req, parsed, config, ctx.request_id, ctx.client_ip);
                        return;
                    }
                    apply_http_response(run_responses_compact_gateway(req, config, ctx.request_id), res);
                }));

    server.Get("/api/admin/dashboard", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return admin_dashboard_http_response(ctx.raw_request, config, ctx.request_id);
               }));
    server.Get("/api/admin/request", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return admin_usage_page_http_response(ctx.raw_request, config, ctx.request_id, ctx.parsed.target);
               }));
    server.Get("/api/admin/request/timeseries", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return admin_usage_timeseries_http_response(ctx.raw_request, config, ctx.request_id,
                                                               ctx.parsed.target);
               }));
    server.Get("/api/admin/request/events/:event_id/detail",
               api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                   const auto event_id = path_param_i64(req, "event_id");
                   return event_id.has_value() ? admin_usage_event_detail_http_response(ctx.raw_request, config,
                                                                                        ctx.request_id, *event_id) :
                                                 api_json_response(api_failure("event_id 无效"), ctx.request_id);
               }));
    server.Get("/api/admin/settings", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return admin_settings_get_response(ctx.raw_request, config, ctx.request_id);
               }));
    server.Put("/api/admin/settings", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                   return admin_settings_put_response(ctx.raw_request, req.body, config, ctx.request_id);
               }));
    server.Get("/api/admin/users", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return admin_list_users_response(ctx.raw_request, config, ctx.request_id);
               }));
    server.Post("/api/admin/users", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                    return admin_create_user_response(ctx.raw_request, req.body, config, ctx.request_id);
                }));
    server.Put("/api/admin/users/:user_id", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                   const auto user_id = path_param_i64(req, "user_id");
                   return user_id.has_value() ?
                              admin_update_user_response(*user_id, ctx.raw_request, req.body, config, ctx.request_id) :
                              api_json_response(api_failure("用户不存在"), ctx.request_id);
               }));
    server.Delete("/api/admin/users/:user_id", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                      const auto user_id = path_param_i64(req, "user_id");
                      return user_id.has_value() ?
                                 admin_delete_user_response(*user_id, ctx.raw_request, config, ctx.request_id) :
                                 api_json_response(api_failure("用户不存在"), ctx.request_id);
                  }));
    server.Post(
        "/api/admin/users/:user_id/password", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
            const auto user_id = path_param_i64(req, "user_id");
            return user_id.has_value() ?
                       admin_reset_user_password_response(*user_id, ctx.raw_request, req.body, config, ctx.request_id) :
                       api_json_response(api_failure("用户不存在"), ctx.request_id);
        }));
    server.Post("/api/admin/users/:user_id/balance", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                    const auto user_id = path_param_i64(req, "user_id");
                    return user_id.has_value() ? admin_add_user_balance_response(*user_id, ctx.raw_request, req.body,
                                                                                 config, ctx.request_id) :
                                                 api_json_response(api_failure("用户不存在"), ctx.request_id);
                }));

    server.Get("/api/billing/balance", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return billing_balance_response(ctx.raw_request, config, ctx.request_id);
               }));

    auto channel_groups = api([&](const ::httplib::Request &req, const RequestContext &ctx) {
        const ChannelGroupsAdminParsedRequest parsed{ ctx.parsed.method, ctx.parsed.path, ctx.parsed.target };
        return channel_groups_admin_route(ctx.raw_request, req.body, parsed, config, ctx.request_id);
    });
    server.Get(R"(/api/admin/channel-groups.*)", channel_groups);
    server.Post(R"(/api/admin/channel-groups.*)", channel_groups);
    server.Put(R"(/api/admin/channel-groups.*)", channel_groups);
    server.Delete(R"(/api/admin/channel-groups.*)", channel_groups);

    auto channels = api([&](const ::httplib::Request &req, const RequestContext &ctx) {
        const ChannelAdminParsedRequest parsed{ ctx.parsed.method, ctx.parsed.path, ctx.parsed.target };
        return channel_admin_route(ctx.raw_request, req.body, parsed, config, ctx.request_id);
    });
    server.Get(R"(/api/channel.*)", channels);
    server.Post(R"(/api/channel.*)", channels);
    server.Put(R"(/api/channel.*)", channels);
    server.Delete(R"(/api/channel.*)", channels);
}

std::string handle_http_request(std::string_view request, const Config &config, bool draining,
                                std::string_view request_id)
{
    InMemoryHttpServer server;
    auto draining_flag = std::make_shared<std::atomic_bool>(draining);
    server.set_keep_alive_max_count(1);
    server.set_payload_max_length(static_cast<size_t>(config.http_max_body_bytes));
    register_http_routes(server, config, draining_flag);

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
        return serialize_response_bytes(
            http_response(400, "Bad Request", "bad request\n", "text/plain; charset=utf-8", request_id));
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
