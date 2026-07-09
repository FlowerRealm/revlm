#include "server/http_dispatch.hpp"

#include "auth/security.hpp"
#include "auth/users.hpp"
#include "billing/billing.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "config/app_settings.hpp"
#include "models/models.hpp"
#include "proxy_request/gateway.hpp"
#include "proxy_request/gateway_resilience.hpp"
#include "proxy_request/responses_proxy.hpp"
#include "proxy_request/token_auth.hpp"
#include "runtime/runtime_workers.hpp"
#include "server/tokens.hpp"
#include "store/mysql.hpp"
#include "usage/admin_usage_api.hpp"
#include "usage/user_usage_api.hpp"
#include "util/json_util.hpp"
#include "util/user_input.hpp"

#include <boost/json.hpp>
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include "util/strings.hpp"

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

struct JsonField {
    std::string name;
    std::string value;
};

struct JsonObjectField {
    std::string name;
    std::string raw;
    bool is_string = false;
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

std::string make_request_id()
{
    using clock = std::chrono::steady_clock;
    static std::atomic_uint64_t seq{ 0 };
    std::ostringstream out;
    out << "req-" << clock::now().time_since_epoch().count() << '-' << seq.fetch_add(1);
    return out.str();
}

std::string api_success(std::string_view data_json = {})
{
    boost::json::object body;
    body["success"] = true;
    body["message"] = "";
    if (data_json.empty()) {
        return boost::json::serialize(body) + "\n";
    }
    boost::system::error_code ec;
    boost::json::value data = boost::json::parse(data_json, ec);
    body["data"] = ec ? boost::json::value() : std::move(data);
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

std::string user_json(const User &user, bool include_mode)
{
    boost::json::object body;
    body["id"] = user.id;
    body["email"] = user.email;
    body["username"] = user.username;
    body["role"] = user.role;
    body["status"] = user.status;
    if (include_mode) {
        body["mode"] = "business";
    }
    return boost::json::serialize(body);
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

boost::json::value optional_string_value(const std::optional<std::string> &value)
{
    return value.has_value() ? boost::json::value(*value) : boost::json::value(nullptr);
}

boost::json::value non_empty_string_value(std::string_view value)
{
    const std::string trimmed = trim_ascii(value);
    return trimmed.empty() ? boost::json::value(nullptr) : boost::json::value(std::move(trimmed));
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

bool parse_json_object_strings(std::string_view json, std::vector<JsonField> &fields)
{
    std::vector<std::pair<std::string, std::string>> parsed;
    if (!parse_json_object_string_fields(json, parsed)) {
        return false;
    }
    fields.clear();
    fields.reserve(parsed.size());
    for (auto &[name, value] : parsed) {
        fields.push_back(JsonField{ std::move(name), std::move(value) });
    }
    return true;
}

bool parse_json_object_fields(std::string_view json, std::vector<JsonObjectField> &fields)
{
    std::vector<std::tuple<std::string, std::string, bool>> parsed;
    if (!parse_json_object_mixed_fields(json, parsed)) {
        return false;
    }
    fields.clear();
    fields.reserve(parsed.size());
    for (auto &[name, raw, is_string] : parsed) {
        fields.push_back(JsonObjectField{
            .name = std::move(name),
            .raw = std::move(raw),
            .is_string = is_string,
        });
    }
    return true;
}

std::string json_field(const std::vector<JsonField> &fields, std::string_view name)
{
    for (const JsonField &field : fields) {
        if (field.name == name) {
            return field.value;
        }
    }
    return {};
}

std::optional<JsonObjectField> json_object_field(const std::vector<JsonObjectField> &fields, std::string_view name)
{
    for (const JsonObjectField &field : fields) {
        if (field.name == name) {
            return field;
        }
    }
    return std::nullopt;
}

std::string admin_settings_json(const AdminSettingsSnapshot &settings)
{
    boost::json::object body;
    body["mode"] = settings.mode;
    body["site_base_url"] = settings.site_base_url;
    body["site_base_url_override"] = settings.site_base_url_override;
    body["site_base_url_effective"] = settings.site_base_url_effective;
    body["site_base_url_invalid"] = settings.site_base_url_invalid;
    body["billing_paygo_price_multiplier"] = settings.billing_paygo_price_multiplier;
    body["billing_paygo_price_multiplier_override"] = settings.billing_paygo_price_multiplier_override;
    return boost::json::serialize(body);
}

HttpResponse api_json_response(std::string body, std::string_view request_id, const std::vector<Header> &headers = {})
{
    return http_response(200, "OK", body, "application/json; charset=utf-8", request_id, headers);
}

bool parse_json_model_mappings_array(std::string_view json, std::vector<TokenModelMappingCreate> &out)
{
    out.clear();
    const auto doc = parse_json(json);
    if (!doc || !doc->is_array()) {
        return false;
    }
    for (const auto &item : doc->as_array()) {
        if (!item.is_object()) {
            return false;
        }
        std::string input_model;
        std::string target_model;
        for (const auto &field : item.as_object()) {
            if (!field.value().is_string()) {
                continue;
            }
            const std::string value{ field.value().as_string().data(), field.value().as_string().size() };
            if (field.key() == "input_model") {
                input_model = value;
            } else if (field.key() == "target_model") {
                target_model = value;
            }
        }
        out.push_back(TokenModelMappingCreate{
            .input_model = std::move(input_model),
            .target_model = std::move(target_model),
        });
    }
    return true;
}

std::string model_icon_url(std::string_view model_id, std::string_view owned_by)
{
    const std::string haystack = lowercase_ascii(trim_ascii(owned_by) + " " + trim_ascii(model_id));
    struct IconRule {
        const char *key;
        const char *icon;
    };
    static constexpr IconRule rules[] = {
        { "openai", "openai" },
        { "gpt", "openai" },
        { "dalle", "openai" },
        { "whisper", "openai" },
        { "anthropic", "claude-color" },
        { "claude", "claude-color" },
        { "moonshot", "kimi-color" },
        { "kimi", "kimi-color" },
        { "zhipu", "zhipu-color" },
        { "chatglm", "zhipu-color" },
        { "glm", "zhipu-color" },
        { "qwen", "qwen-color" },
        { "tongyi", "qwen-color" },
        { "deepseek", "deepseek-color" },
        { "minimax", "minimax-color" },
        { "wenxin", "wenxin-color" },
        { "ernie", "wenxin-color" },
        { "baidu", "baidu-color" },
        { "spark", "spark-color" },
        { "xunfei", "spark-color" },
        { "iflytek", "spark-color" },
        { "hunyuan", "hunyuan-color" },
        { "tencent", "tencent-color" },
        { "doubao", "doubao-color" },
        { "bytedance", "bytedance-color" },
        { "volcengine", "bytedance-color" },
        { "mistral", "mistral-color" },
        { "cohere", "cohere-color" },
        { "bedrock", "bedrock-color" },
        { "aws", "aws-color" },
        { "azureai", "azureai-color" },
        { "azure", "azure-color" },
        { "cloudflare", "cloudflare-color" },
        { "openrouter", "openrouter" },
        { "ollama", "ollama" },
        { "xai", "xai" },
        { "perplexity", "perplexity-color" },
        { "replicate", "replicate" },
    };
    for (const IconRule &rule : rules) {
        if (haystack.find(rule.key) != std::string::npos) {
            return std::string{ "/assets/model-icons/" } + rule.icon + ".svg";
        }
    }
    return {};
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

std::vector<Model> token_alias_catalog_items(const TokenAuth &auth, const std::vector<Model> &targets)
{
    if (auth.model_mappings.empty() || targets.empty()) {
        return {};
    }
    std::unordered_map<std::string, std::string> owned_by_by_model;
    owned_by_by_model.reserve(targets.size());
    for (const Model &target : targets) {
        const std::string public_id = trim_ascii(target.name);
        if (!public_id.empty()) {
            owned_by_by_model.emplace(public_id, owned_by_for_model_item(target));
        }
    }

    std::vector<std::string> aliases;
    aliases.reserve(auth.model_mappings.size());
    for (const auto &[input_raw, target_raw] : auth.model_mappings) {
        const std::string input = trim_ascii(input_raw);
        const std::string target = trim_ascii(target_raw);
        if (input.empty() || target.empty() || input == target) {
            continue;
        }
        if (!owned_by_by_model.contains(target)) {
            continue;
        }
        if (owned_by_by_model.contains(input)) {
            continue;
        }
        aliases.push_back(input);
    }
    std::sort(aliases.begin(), aliases.end());

    std::vector<Model> out;
    out.reserve(aliases.size());
    for (const std::string &alias : aliases) {
        out.push_back(Model{ 0, alias, owned_by_by_model[trim_ascii(auth.model_mappings.at(alias))], 0, 0, 0, 0, 0 });
    }
    return out;
}

class RegistrationLock {
public:
    explicit RegistrationLock(MysqlConnection &conn)
        : conn_(conn)
    {
        const std::string got =
            trim_ascii(conn_.query_one("SELECT GET_LOCK(" + conn_.quote("revlm.users.register") + ", 5)").value_or(""));
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
            (void)conn_.query_one("SELECT RELEASE_LOCK(" + conn_.quote("revlm.users.register") + ")");
        } catch (const std::exception &) {
        }
    }

private:
    MysqlConnection &conn_;
    bool locked_ = false;
};

HttpResponse register_response(std::string_view raw_request, std::string_view body, const Config &config,
                               std::string_view request_id)
{
    std::vector<JsonField> fields;
    if (!parse_json_object_strings(body, fields)) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    try {
        const std::string email = normalize_email(json_field(fields, "email"));
        const std::string username = normalize_username(json_field(fields, "username"));
        const std::string password = json_field(fields, "password");
        if (password.empty()) {
            return api_json_response(api_failure("邮箱或密码不能为空"), request_id);
        }
        const std::string password_hash = hash_password(password);

        MysqlConnection conn(config.db_dsn);
        UserStore store(conn);
        RegistrationLock lock(conn);
        const std::string role = store.count_users() == 0 ? "root" : "user";
        User user(email, username, password_hash, role);
        user.id = store.create_user(user);
        user.status = 1;
        const SessionCookie session = make_session_cookie(user.id, session_secret_for_config(config));
        store.upsert_session_binding_payload(user.id, session_binding_hash(session.key), "web",
                                             mysql_datetime_from_unix(session.expires_unix));
        return api_json_response(api_success(user_json(user, false)), request_id,
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
    std::vector<JsonField> fields;
    if (!parse_json_object_strings(body, fields)) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    std::string login = trim_ascii(json_field(fields, "login"));
    if (login.empty()) {
        login = trim_ascii(json_field(fields, "username"));
    }
    if (login.empty()) {
        login = trim_ascii(json_field(fields, "email"));
    }
    const std::string password = json_field(fields, "password");
    if (login.empty() || password.empty()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    try {
        MysqlConnection conn(config.db_dsn);
        UserStore store(conn);
        std::optional<User> user = store.get_user_by_email(lowercase_ascii(login));
        if (!user.has_value()) {
            user = store.get_user_by_username(login);
        }
        if (!user.has_value() || user->status != 1 || !check_password(user->password_hash, password)) {
            return api_json_response(api_failure("邮箱/账号名或密码错误"), request_id);
        }
        const SessionCookie session = make_session_cookie(user->id, session_secret_for_config(config));
        store.upsert_session_binding_payload(user->id, session_binding_hash(session.key), "web",
                                             mysql_datetime_from_unix(session.expires_unix));
        return api_json_response(api_success(user_json(*user, false)), request_id,
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
    return api_json_response(api_success(user_json(*user, true)), request_id);
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
        MysqlConnection conn(config.db_dsn);
        UserStore store(conn);
        store.delete_session_binding(user->id, binding_hash);
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
        MysqlConnection conn(config.db_dsn);
        TokenStore store(conn);
        const std::vector<UserToken> tokens = store.list_user_tokens(user->id);
        boost::json::array data;
        for (const UserToken &token : tokens) {
            boost::json::object item;
            item["id"] = token.id;
            item["name"] = optional_string_value(token.name);
            item["status"] = token.status;
            data.push_back(std::move(item));
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
        std::vector<JsonField> fields;
        if (!parse_json_object_strings(body, fields)) {
            return api_json_response(api_failure("无效的参数"), request_id);
        }
        std::string name = trim_ascii(json_field(fields, "name"));
        if (!name.empty()) {
            token_name = name;
        }
    }

    try {
        const std::string raw_token = new_random_token("sk_", 32);
        MysqlConnection conn(config.db_dsn);
        TokenStore store(conn);
        const long long token_id = store.create_user_token(user->id, token_name, raw_token);
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
        MysqlConnection conn(config.db_dsn);
        TokenStore store(conn);
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
        MysqlConnection conn(config.db_dsn);
        TokenStore store(conn);
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
        MysqlConnection conn(config.db_dsn);
        TokenStore store(conn);
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
        MysqlConnection conn(config.db_dsn);
        TokenStore store(conn);
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
        MysqlConnection conn(config.db_dsn);
        TokenStore store(conn);
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
            boost::json::object item;
            item["name"] = name;
            item["description"] = optional_string_value(group.description);
            item["status"] = group.status;
            item["price_multiplier"] = group.price_multiplier;
            allowed_json.push_back(std::move(item));
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

    std::vector<JsonObjectField> body_fields;
    if (!parse_json_object_fields(body, body_fields)) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    const auto groups_field = json_object_field(body_fields, "channel_groups");
    if (!groups_field.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    std::vector<std::string> channel_groups;
    if (!parse_json_string_array(groups_field->raw, channel_groups)) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    try {
        MysqlConnection conn(config.db_dsn);
        TokenStore store(conn);
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

HttpResponse token_model_mappings_response(std::string_view raw_request, const Config &config,
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
        MysqlConnection conn(config.db_dsn);
        TokenStore store(conn);
        if (!store.get_user_token_by_id(user->id, token_id).has_value()) {
            return api_json_response(api_failure("令牌不存在"), request_id);
        }

        const std::vector<TokenModelTargetOption> targets = store.list_token_model_mapping_targets(token_id);
        const std::vector<TokenModelMapping> mappings = store.list_token_model_mappings(token_id);
        std::unordered_set<std::string> allowed;

        boost::json::array targets_json;
        for (const TokenModelTargetOption &target : targets) {
            const std::string public_id = trim_ascii(target.public_id);
            if (public_id.empty()) {
                continue;
            }
            allowed.insert(public_id);
            const std::string icon_url = model_icon_url(public_id, target.owned_by);
            boost::json::object item;
            item["public_id"] = public_id;
            item["group_name"] = trim_ascii(target.group_name);
            item["owned_by"] = non_empty_string_value(target.owned_by);
            item["icon_url"] = non_empty_string_value(icon_url);
            targets_json.push_back(std::move(item));
        }

        boost::json::array mappings_json;
        for (const TokenModelMapping &mapping : mappings) {
            const std::string input = trim_ascii(mapping.input_model);
            const std::string target = trim_ascii(mapping.target_model);
            if (input.empty() || target.empty() || !allowed.contains(target)) {
                continue;
            }
            boost::json::object item;
            item["input_model"] = input;
            item["target_model"] = target;
            mappings_json.push_back(std::move(item));
        }

        boost::json::object data;
        data["token_id"] = token_id;
        data["available_target_models"] = std::move(targets_json);
        data["mappings"] = std::move(mappings_json);
        return api_json_response(api_success(std::move(data)), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询模型映射失败"), request_id);
    }
}

HttpResponse replace_token_model_mappings_response(std::string_view raw_request, const Config &config,
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

    std::vector<JsonObjectField> body_fields;
    if (!parse_json_object_fields(body, body_fields)) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    const auto mappings_field = json_object_field(body_fields, "mappings");
    if (!mappings_field.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    std::vector<TokenModelMappingCreate> mappings;
    if (!parse_json_model_mappings_array(mappings_field->raw, mappings)) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    try {
        MysqlConnection conn(config.db_dsn);
        TokenStore store(conn);
        if (!store.get_user_token_by_id(user->id, token_id).has_value()) {
            return api_json_response(api_failure("令牌不存在"), request_id);
        }
        if (!store.replace_token_model_mappings(token_id, mappings)) {
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

    std::vector<JsonField> fields;
    if (!parse_json_object_strings(body, fields)) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    const std::string current_password = json_field(fields, "current_password");
    if (current_password.empty()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    try {
        const std::string email = normalize_email(json_field(fields, "email"));
        MysqlConnection conn(config.db_dsn);
        UserStore store(conn);
        DbTransaction tr(conn);
        const auto locked_user = store.get_user_by_id_for_update(user->id);
        if (!locked_user.has_value()) {
            return api_json_response(api_failure("未登录"), request_id,
                                     { Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
        }
        if (locked_user->status != 1) {
            return api_json_response(api_failure("账号已被禁用"), request_id,
                                     { Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
        }
        if (!check_password(locked_user->password_hash, current_password)) {
            return api_json_response(api_failure("旧密码错误"), request_id);
        }
        store.update_user_email(user->id, email);
        store.delete_all_session_bindings(user->id);
        tr.commit();
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

    std::vector<JsonField> fields;
    if (!parse_json_object_strings(body, fields)) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    const std::string old_password = json_field(fields, "old_password");
    const std::string new_password = json_field(fields, "new_password");
    if (old_password.empty() || new_password.empty()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    try {
        const std::string password_hash = hash_password(new_password);
        MysqlConnection conn(config.db_dsn);
        UserStore store(conn);
        DbTransaction tr(conn);
        const auto locked_user = store.get_user_by_id_for_update(user->id);
        if (!locked_user.has_value()) {
            return api_json_response(api_failure("未登录"), request_id,
                                     { Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
        }
        if (locked_user->status != 1) {
            return api_json_response(api_failure("账号已被禁用"), request_id,
                                     { Header{ "Set-Cookie", clear_session_cookie_header(raw_request) } });
        }
        if (!check_password(locked_user->password_hash, old_password)) {
            return api_json_response(api_failure("旧密码错误"), request_id);
        }
        store.update_user_password_hash(user->id, password_hash);
        store.delete_all_session_bindings(user->id);
        tr.commit();
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
        const std::vector<Model> &targets = ModelManager::instance().models();

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

        for (const Model &item : targets) {
            append_item(item);
        }
        for (const Model &item : token_alias_catalog_items(auth, targets)) {
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
    const TokenAuth &auth = *auth_result.auth;

    const std::string response_id = trim_ascii(requested_model_id);
    if (response_id.empty()) {
        return http_response(404, "Not Found", "not found\n", "text/plain; charset=utf-8", request_id);
    }
    std::string resolved_id = response_id;
    if (const auto mapped = resolve_model_mapping(auth, response_id); mapped.second) {
        resolved_id = mapped.first;
    }

    try {
        const std::vector<Model> &models = ModelManager::instance().models();
        const auto model_it = std::ranges::find(models, resolved_id, &Model::name);
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
        MysqlConnection conn(config.db_dsn);
        AppSettingsStore store(conn);
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

    std::vector<JsonField> fields;
    if (!parse_json_object_strings(body, fields)) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    try {
        MysqlConnection conn(config.db_dsn);
        AppSettingsStore store(conn);
        store.update_admin_settings(AdminSettingsUpdate{
            .site_base_url = json_field(fields, "site_base_url"),
            .billing_paygo_price_multiplier = json_field(fields, "billing_paygo_price_multiplier"),
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

std::string admin_user_json(const AdminUserView &user)
{
    boost::json::object body;
    body["id"] = user.id;
    body["email"] = user.email;
    body["username"] = user.username;
    body["role"] = user.role;
    body["status"] = user.status;
    body["balance_usd"] = user.balance_usd;
    body["created_at"] = user.created_at;
    return boost::json::serialize(body);
}

std::string admin_users_json(const std::vector<AdminUserView> &users)
{
    std::string body = "[";
    for (size_t i = 0; i < users.size(); ++i) {
        if (i > 0) {
            body += ",";
        }
        body += admin_user_json(users[i]);
    }
    body += "]";
    return body;
}

std::optional<AdminUserUpdateInput> parse_admin_user_update_body(std::string_view raw_body)
{
    std::vector<JsonField> fields;
    if (!parse_json_object_strings(raw_body, fields)) {
        return std::nullopt;
    }
    AdminUserUpdateInput input;
    bool has_invalid_status = false;
    for (const JsonField &field : fields) {
        if (field.name == "email") {
            input.email = field.value;
        } else if (field.name == "role") {
            input.role = field.value;
        } else if (field.name == "status") {
            const auto parsed = parse_json_int_scalar(field.value);
            if (!parsed.has_value()) {
                has_invalid_status = true;
                break;
            }
            input.status = *parsed;
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
        MysqlConnection conn(config.db_dsn);
        UserStore store(conn);
        return api_json_response(api_success(admin_users_json(store.list_admin_users())), request_id);
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
    std::vector<JsonField> fields;
    if (!parse_json_object_strings(body, fields)) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    try {
        const std::string email = normalize_email(json_field(fields, "email"));
        const std::string username = normalize_username(json_field(fields, "username"));
        const std::string password = json_field(fields, "password");
        if (password.empty()) {
            return api_json_response(api_failure("邮箱或密码不能为空"), request_id);
        }
        const std::string role = normalize_user_role(json_field(fields, "role"), "user");
        const std::string password_hash = hash_password(password);
        MysqlConnection conn(config.db_dsn);
        UserStore store(conn);
        if (store.get_user_by_username(username).has_value()) {
            return api_json_response(api_failure("账号名已被占用"), request_id);
        }
        const long long user_id = store.create_user(User(email, username, password_hash, role));
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
        MysqlConnection conn(config.db_dsn);
        UserStore store(conn);
        DbTransaction tr(conn);
        const auto target = store.get_user_by_id_for_update(user_id);
        if (!target.has_value()) {
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
            const std::string email = normalize_email(*update->email);
            if (email != target->email) {
                store.update_user_email(user_id, email);
            }
        }
        if (update->status.has_value()) {
            const int status = normalize_user_status(*update->status);
            if (status != target->status) {
                store.set_user_status(user_id, status);
            }
        }
        if (update->role.has_value()) {
            const std::string role = normalize_user_role(*update->role, target->role);
            if (role != target->role) {
                store.set_user_role(user_id, role);
            }
        }
        tr.commit();

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
    std::vector<JsonField> fields;
    if (!parse_json_object_strings(body, fields)) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    try {
        const std::string password = json_field(fields, "password");
        if (trim_ascii(password).empty()) {
            return api_json_response(api_failure("新密码不能为空"), request_id);
        }
        MysqlConnection conn(config.db_dsn);
        UserStore store(conn);
        if (!store.reset_user_password_hash(user_id, hash_password(password))) {
            return api_json_response(api_failure("用户不存在"), request_id);
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
    std::vector<JsonField> fields;
    if (!parse_json_object_strings(body, fields)) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    try {
        MysqlConnection conn(config.db_dsn);
        UserStore store(conn);
        const auto balance = store.add_user_balance_usd(user_id, json_field(fields, "amount_usd"));
        if (!balance.has_value()) {
            return api_json_response(api_failure("用户不存在"), request_id);
        }
        return api_json_response(api_success(boost::json::object{ { "balance_usd", *balance } }), request_id);
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
        MysqlConnection conn(config.db_dsn);
        UserStore store(conn);
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
        MysqlConnection conn(config.db_dsn);
        BillingStore store(conn);
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
    std::string request_id = req.get_header_value("X-Request-Id");
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
        RequestInFlightGuard request_guard;
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

} // namespace

void register_http_routes(::httplib::Server &server, const Config &config, const BuildInfo &build,
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
                   return http_response(200, "OK", runtime_metrics_prometheus_text(),
                                        "text/plain; version=0.0.4; charset=utf-8", ctx.request_id);
               }));
    server.Get("/api/meta", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   boost::json::object meta;
                   meta["version"] = build.version;
                   meta["build_date"] = build.date;
                   return http_response(200, "OK", boost::json::serialize(meta) + "\n",
                                        "application/json; charset=utf-8", ctx.request_id);
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
    server.Get("/api/usage/windows", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return usage_windows_http_response(ctx.raw_request, config, ctx.request_id, ctx.parsed.target);
               }));
    server.Get("/api/usage/events", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return usage_events_http_response(ctx.raw_request, config, ctx.request_id, ctx.parsed.target);
               }));
    server.Get("/api/usage/timeseries", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return usage_timeseries_http_response(ctx.raw_request, config, ctx.request_id, ctx.parsed.target);
               }));
    server.Get("/api/usage/events/:event_id/detail", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
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
    server.Get("/api/token/:token_id/model-mappings",
               api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ?
                              token_model_mappings_response(ctx.raw_request, config, ctx.request_id, *token_id) :
                              api_json_response(api_failure("token_id 不合法"), ctx.request_id);
               }));
    server.Put("/api/token/:token_id/model-mappings",
               api([&](const ::httplib::Request &req, const RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ?
                              replace_token_model_mappings_response(ctx.raw_request, config, ctx.request_id, *token_id,
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
                        ctx.raw_request, ctx.parsed.method, ctx.parsed.path, config, build, ctx.request_id,
                        ::revlm::parse_json_bool_field(req.body, "stream").value_or(false) ?
                            ResponsesProxyExecuteOptions{ .write_client = {}, .stream_response = &res } :
                            ResponsesProxyExecuteOptions{});
                    if (!result.handled_stream) {
                        apply_http_response(result.response, res);
                    }
                }));
    server.Post("/v1/responses/input_tokens", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                    return handle_responses_proxy_request(ctx.raw_request, ctx.parsed.method, ctx.parsed.path, config,
                                                          build, ctx.request_id)
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
    server.Get("/api/admin/usage", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return admin_usage_page_http_response(ctx.raw_request, config, ctx.request_id, ctx.parsed.target);
               }));
    server.Get("/api/admin/usage/timeseries", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return admin_usage_timeseries_http_response(ctx.raw_request, config, ctx.request_id,
                                                               ctx.parsed.target);
               }));
    server.Get("/api/admin/usage/users/suggest", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return admin_usage_users_suggest_http_response(ctx.raw_request, config, ctx.request_id,
                                                                  ctx.parsed.target);
               }));
    server.Get("/api/admin/usage/channels/suggest", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return admin_usage_channels_suggest_http_response(ctx.raw_request, config, ctx.request_id,
                                                                     ctx.parsed.target);
               }));
    server.Get("/api/admin/usage/models/suggest", api([&](const ::httplib::Request &, const RequestContext &ctx) {
                   return admin_usage_models_suggest_http_response(ctx.raw_request, config, ctx.request_id,
                                                                   ctx.parsed.target);
               }));
    server.Get(
        "/api/admin/usage/events/:event_id/detail", api([&](const ::httplib::Request &req, const RequestContext &ctx) {
            const auto event_id = path_param_i64(req, "event_id");
            return event_id.has_value() ?
                       admin_usage_event_detail_http_response(ctx.raw_request, config, ctx.request_id, *event_id) :
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

std::string handle_http_request(std::string_view request, const Config &config, const BuildInfo &build, bool draining,
                                std::string_view request_id)
{
    InMemoryHttpServer server;
    auto draining_flag = std::make_shared<std::atomic_bool>(draining);
    server.set_keep_alive_max_count(1);
    server.set_payload_max_length(static_cast<size_t>(config.http_max_body_bytes));
    register_http_routes(server, config, build, draining_flag);

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
