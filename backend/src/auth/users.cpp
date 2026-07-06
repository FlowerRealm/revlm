#include "auth/users.hpp"

#include "util/strings.hpp"
#include "util/user_input.hpp"

#include "auth/crypto.hpp"
#include "auth/security.hpp"

#include <crypt.h>

#include <cctype>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace revlm
{
namespace
{

constexpr std::string_view session_cookie_name = "revlm_session";
constexpr int session_cookie_max_age = 2592000;
constexpr int session_cookie_ttl_seconds = session_cookie_max_age;

std::string_view header_value_from_request(std::string_view request, std::string_view name);

long long unix_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string bcrypt_salt()
{
    static constexpr char alphabet[] = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    const std::string raw = random_bytes(16);
    std::string salt = "$2b$12$";
    int bits = 0;
    unsigned int acc = 0;
    for (unsigned char ch : raw) {
        acc |= static_cast<unsigned int>(ch) << bits;
        bits += 8;
        while (bits >= 6 && salt.size() < 29) {
            salt.push_back(alphabet[acc & 0x3f]);
            acc >>= 6;
            bits -= 6;
        }
    }
    if (salt.size() < 29) {
        salt.push_back(alphabet[acc & 0x3f]);
    }
    salt.resize(29, '.');
    return salt;
}

bool is_https_request(std::string_view raw_request)
{
    const std::string_view remote_ip = header_value_from_request(raw_request, "x-revlm-remote-ip");
    const bool trust_proxy = !remote_ip.empty() && is_trusted_proxy_ipv4(remote_ip, default_trusted_proxies());
    if (trust_proxy) {
        if (const auto forwarded = trusted_forwarded_proto(header_value_from_request(raw_request, "x-forwarded-proto"));
            forwarded.has_value()) {
            return *forwarded == "https";
        }
    }
    for (std::string_view header_name : { "origin", "referer" }) {
        std::string_view value = header_value_from_request(raw_request, header_name);
        if (value.starts_with("https://")) {
            return true;
        }
    }
    return false;
}

std::string_view header_value_from_request(std::string_view request, std::string_view name)
{
    size_t line_start = 0;
    while (line_start < request.size()) {
        const size_t line_end = request.find("\r\n", line_start);
        if (line_end == std::string_view::npos || line_end == line_start) {
            break;
        }
        std::string_view line = request.substr(line_start, line_end - line_start);
        line_start = line_end + 2;
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        std::string_view key = line.substr(0, colon);
        if (key.size() != name.size()) {
            continue;
        }
        bool equal = true;
        for (size_t i = 0; i < key.size(); ++i) {
            char a = key[i];
            char b = name[i];
            if (a >= 'A' && a <= 'Z') {
                a = static_cast<char>(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = static_cast<char>(b - 'A' + 'a');
            }
            if (a != b) {
                equal = false;
                break;
            }
        }
        if (!equal) {
            continue;
        }
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
            value.remove_suffix(1);
        }
        return value;
    }
    return {};
}

std::optional<User> row_to_user(const MysqlResultRow &row)
{
    if (row.size() < 7 || !row[0].has_value()) {
        return std::nullopt;
    }
    User user;
    user.id = std::stoll(*row[0]);
    user.email = row[1].value_or("");
    user.username = row[2].value_or("");
    user.password_hash = row[3].value_or("");
    user.role = row[4].value_or("");
    user.status = std::stoi(row[5].value_or("0"));
    user.created_at = row[6].value_or("");
    return user;
}

std::string format_timestamp_minute(std::string_view raw)
{
    std::string text = trim_ascii(raw);
    if (text.size() >= 16) {
        return text.substr(0, 16);
    }
    return text;
}

long long exec_row_count(MysqlConnection &conn, const std::string &sql)
{
    conn.exec(sql);
    return std::stoll(conn.query_one("SELECT ROW_COUNT()").value_or("-1"));
}

void quoted_positive_user_id(long long user_id, MysqlConnection &conn)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    if (user_id <= 0) {
        return;
    }
    if (!conn.query_one("SELECT id FROM users WHERE id=" + std::to_string(user_id)).has_value()) {
        throw std::runtime_error("用户不存在");
    }
}

bool mysql_table_exists(MysqlConnection &conn, std::string_view table)
{
    return conn.query_one("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
                          "AND table_name=" +
                          conn.quote(table))
               .value_or("0") != "0";
}

std::optional<AdminUserView> row_to_admin_user_view(const MysqlResultRow &row)
{
    if (row.size() < 7 || !row[0].has_value()) {
        return std::nullopt;
    }
    AdminUserView user;
    user.id = std::stoll(*row[0]);
    user.email = row[1].value_or("");
    user.username = row[2].value_or("");
    user.role = row[3].value_or("");
    user.status = std::stoi(row[4].value_or("0"));
    user.balance_usd = format_usd_plain_fixed6(row[5].value_or("0"));
    user.created_at = format_timestamp_minute(row[6].value_or(""));
    return user;
}

std::optional<SessionBinding> row_to_session_binding(const MysqlResultRow &row)
{
    if (row.size() < 4 || !row[0].has_value()) {
        return std::nullopt;
    }
    SessionBinding binding;
    binding.user_id = std::stoll(*row[0]);
    binding.route_key_hash = row[1].value_or("");
    binding.payload_json = row[2].value_or("");
    binding.expires_at = row[3].value_or("");
    return binding;
}

} // namespace

std::string hash_password(std::string_view password)
{
    require_password_length(password);
    const std::string salt = bcrypt_salt();
    crypt_data data{};
    data.initialized = 0;
    char *hash = ::crypt_r(std::string{ password }.c_str(), salt.c_str(), &data);
    if (hash == nullptr || std::strncmp(hash, "$2", 2) != 0) {
        throw std::runtime_error("密码哈希失败");
    }
    return std::string{ hash };
}

bool check_password(std::string_view hash, std::string_view password)
{
    if (!hash.starts_with("$2")) {
        return false;
    }
    crypt_data data{};
    data.initialized = 0;
    char *got = ::crypt_r(std::string{ password }.c_str(), std::string{ hash }.c_str(), &data);
    if (got == nullptr) {
        return false;
    }
    const std::string got_text{ got };
    return constant_time_equal(got_text, hash);
}

SessionCookie make_session_cookie(long long user_id, std::string_view secret)
{
    assert(user_id > 0 && "internal: user id must be positive");
    if (user_id <= 0) {
        return {};
    }
    SessionCookie session;
    session.user_id = user_id;
    session.expires_unix = unix_now() + session_cookie_ttl_seconds;
    session.key = base64url_encode(random_bytes(24));
    const std::string payload =
        std::to_string(session.user_id) + "|" + std::to_string(session.expires_unix) + "|" + session.key;
    const std::string payload64 = base64url_encode(payload);
    const std::string sig64 = base64url_encode(hmac_sha256(secret, payload64));
    session.value = payload64 + "." + sig64;
    return session;
}

std::optional<SessionCookie> verify_session_cookie_value(std::string_view cookie_value, std::string_view secret)
{
    const size_t dot = cookie_value.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= cookie_value.size()) {
        return std::nullopt;
    }
    const std::string_view payload64 = cookie_value.substr(0, dot);
    const std::string_view sig64 = cookie_value.substr(dot + 1);
    const std::string want = base64url_encode(hmac_sha256(secret, payload64));
    if (!constant_time_equal(want, sig64)) {
        return std::nullopt;
    }
    const auto payload = base64url_decode(payload64);
    if (!payload.has_value()) {
        return std::nullopt;
    }
    const size_t first = payload->find('|');
    const size_t second = first == std::string::npos ? std::string::npos : payload->find('|', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
        return std::nullopt;
    }
    const auto user_id = parse_positive_i64_or(std::string_view{ *payload }.substr(0, first));
    const auto expires = parse_positive_i64_or(std::string_view{ *payload }.substr(first + 1, second - first - 1));
    if (!user_id.has_value() || !expires.has_value() || *expires <= unix_now()) {
        return std::nullopt;
    }
    std::string key{ std::string_view{ *payload }.substr(second + 1) };
    if (key.size() < 16) {
        return std::nullopt;
    }
    SessionCookie session;
    session.user_id = *user_id;
    session.expires_unix = *expires;
    session.key = std::move(key);
    session.value = std::string{ cookie_value };
    return session;
}

std::string session_binding_hash(std::string_view session_key)
{
    return sha256_hex(session_key);
}

std::string session_secret_for_config(const Config &config)
{
    if (!trim_ascii(config.session_secret).empty()) {
        return config.session_secret;
    }
    throw std::runtime_error("SESSION_SECRET must not be empty");
}

std::optional<std::string> cookie_value(std::string_view raw_request, std::string_view name)
{
    std::string_view cookie = header_value_from_request(raw_request, "cookie");
    while (!cookie.empty()) {
        while (!cookie.empty() && (cookie.front() == ' ' || cookie.front() == ';')) {
            cookie.remove_prefix(1);
        }
        const size_t semicolon = cookie.find(';');
        std::string_view part = semicolon == std::string_view::npos ? cookie : cookie.substr(0, semicolon);
        const size_t eq = part.find('=');
        if (eq != std::string_view::npos && trim_ascii(part.substr(0, eq)) == name) {
            return std::string{ part.substr(eq + 1) };
        }
        if (semicolon == std::string_view::npos) {
            break;
        }
        cookie.remove_prefix(semicolon + 1);
    }
    return std::nullopt;
}

std::string set_session_cookie_header(std::string_view value, std::string_view raw_request)
{
    std::string header = std::string{ session_cookie_name } + "=" + std::string{ value } +
                         "; Path=/; Max-Age=" + std::to_string(session_cookie_max_age) + "; HttpOnly; SameSite=Strict";
    if (is_https_request(raw_request)) {
        header += "; Secure";
    }
    return header;
}

std::string clear_session_cookie_header(std::string_view raw_request)
{
    std::string header = std::string{ session_cookie_name } + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict";
    if (is_https_request(raw_request)) {
        header += "; Secure";
    }
    return header;
}

UserStore::UserStore(MysqlConnection &conn)
    : conn_(conn)
{
}

long long UserStore::count_users()
{
    return std::stoll(conn_.query_one("SELECT COUNT(*) FROM users").value_or("0"));
}

long long UserStore::create_user(const CreateUserInput &input)
{
    const std::string sql = "INSERT INTO users(email, username, password_hash, role, status, created_at) VALUES(" +
                            conn_.quote(input.email) + ", " + conn_.quote(input.username) + ", " +
                            conn_.quote(input.password_hash) + ", " + conn_.quote(input.role) +
                            ", 1, CURRENT_TIMESTAMP)";
    conn_.exec(sql);
    return static_cast<long long>(conn_.last_insert_id());
}

std::optional<User> UserStore::get_user_by_id(long long id)
{
    assert(id > 0 && "internal: user id must be positive");
    if (id <= 0) {
        return std::nullopt;
    }
    return get_user_by_sql("SELECT id,email,username,password_hash,role,status,created_at FROM users WHERE id=" +
                           std::to_string(id));
}

std::optional<User> UserStore::get_user_by_id_for_update(long long id)
{
    assert(id > 0 && "internal: user id must be positive");
    if (id <= 0) {
        return std::nullopt;
    }
    return get_user_by_sql("SELECT id,email,username,password_hash,role,status,created_at FROM users WHERE id=" +
                           std::to_string(id) + " FOR UPDATE");
}

std::optional<User> UserStore::get_user_by_email(std::string_view email)
{
    return get_user_by_sql("SELECT id,email,username,password_hash,role,status,created_at FROM users WHERE email=" +
                           conn_.quote(email));
}

std::optional<User> UserStore::get_user_by_username(std::string_view username)
{
    return get_user_by_sql("SELECT id,email,username,password_hash,role,status,created_at FROM users WHERE username=" +
                           conn_.quote(username));
}

std::vector<AdminUserView> UserStore::list_admin_users()
{
    const std::string sql = "SELECT u.id,u.email,u.username,u.role,u.status,COALESCE(b.usd,0),u.created_at "
                            "FROM users u "
                            "LEFT JOIN user_balances b ON b.user_id=u.id "
                            "ORDER BY u.id DESC";
    const auto rows = conn_.query_rows(sql);
    std::vector<AdminUserView> users;
    users.reserve(rows.size());
    for (const auto &row : rows) {
        if (auto user = row_to_admin_user_view(row); user.has_value()) {
            users.push_back(std::move(*user));
        }
    }
    return users;
}

void UserStore::update_user_email(long long user_id, std::string_view email)
{
    assert(user_id > 0 && "internal: user id must be positive");
    if (user_id <= 0) {
        return;
    }
    conn_.exec("UPDATE users SET email=" + conn_.quote(email) + " WHERE id=" + std::to_string(user_id));
    if (conn_.affected_rows() > 1) {
        throw std::runtime_error("update user email affected unexpected row count");
    }
}

void UserStore::update_user_password_hash(long long user_id, std::string_view password_hash)
{
    assert(user_id > 0 && "internal: user id must be positive");
    if (user_id <= 0) {
        return;
    }
    conn_.exec("UPDATE users SET password_hash=" + conn_.quote(password_hash) + " WHERE id=" + std::to_string(user_id));
    if (conn_.affected_rows() != 1) {
        throw std::runtime_error("update user password affected unexpected row count");
    }
}

bool UserStore::reset_user_password_hash(long long user_id, std::string_view password_hash)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    if (user_id <= 0) {
        return false;
    }
    DbTransaction tr(conn_);
    const std::string user_id_sql = std::to_string(user_id);
    if (!conn_.query_one("SELECT id FROM users WHERE id=" + user_id_sql + " FOR UPDATE").has_value()) {
        return false;
    }
    if (exec_row_count(conn_, "UPDATE users SET password_hash=" + conn_.quote(password_hash) +
                                  " WHERE id=" + user_id_sql) != 1) {
        return false;
    }
    tr.commit();
    return true;
}

void UserStore::set_user_role(long long user_id, std::string_view role)
{
    quoted_positive_user_id(user_id, conn_);
    conn_.exec("UPDATE users SET role=" + conn_.quote(role) + " WHERE id=" + std::to_string(user_id));
    if (conn_.affected_rows() > 1) {
        throw std::runtime_error("update user role affected unexpected row count");
    }
}

void UserStore::set_user_status(long long user_id, int status)
{
    quoted_positive_user_id(user_id, conn_);
    conn_.exec("UPDATE users SET status=" + std::to_string(status) + " WHERE id=" + std::to_string(user_id));
    if (conn_.affected_rows() > 1) {
        throw std::runtime_error("update user status affected unexpected row count");
    }
}

std::optional<std::string> UserStore::add_user_balance_usd(long long user_id, std::string_view amount_usd)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    if (user_id <= 0) {
        return std::nullopt;
    }
    const std::string amount = normalize_usd_amount(amount_usd);
    DbTransaction tr(conn_);
    const std::string user_id_sql = std::to_string(user_id);
    if (!conn_.query_one("SELECT id FROM users WHERE id=" + user_id_sql + " FOR UPDATE").has_value()) {
        return std::nullopt;
    }
    conn_.exec("INSERT INTO user_balances(user_id, usd, created_at, updated_at) VALUES(" + user_id_sql + ", " + amount +
               ", CURRENT_TIMESTAMP, CURRENT_TIMESTAMP) "
               "ON DUPLICATE KEY UPDATE usd=usd+VALUES(usd), updated_at=CURRENT_TIMESTAMP");
    const std::string balance = format_usd_plain_fixed6(
        conn_.query_one("SELECT usd FROM user_balances WHERE user_id=" + user_id_sql).value_or("0"));
    tr.commit();
    return balance;
}

bool UserStore::delete_user(long long user_id)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    if (user_id <= 0) {
        return false;
    }
    DbTransaction tr(conn_);
    const std::string user_id_sql = std::to_string(user_id);
    if (!conn_.query_one("SELECT id FROM users WHERE id=" + user_id_sql + " FOR UPDATE").has_value()) {
        return false;
    }
    if (mysql_table_exists(conn_, "subscription_orders")) {
        conn_.exec("DELETE FROM subscription_orders WHERE user_id=" + user_id_sql);
    }
    if (mysql_table_exists(conn_, "user_subscriptions")) {
        conn_.exec("DELETE FROM user_subscriptions WHERE user_id=" + user_id_sql);
    }
    conn_.exec("DELETE FROM usage_events WHERE user_id=" + user_id_sql +
               " OR token_id IN (SELECT id FROM user_tokens WHERE user_id=" + user_id_sql + ")");
    if (mysql_table_exists(conn_, "topup_orders")) {
        conn_.exec("DELETE FROM topup_orders WHERE user_id=" + user_id_sql);
    }
    conn_.exec("DELETE FROM user_balances WHERE user_id=" + user_id_sql);
    conn_.exec("DELETE FROM token_model_mappings WHERE token_id IN "
               "(SELECT id FROM user_tokens WHERE user_id=" +
               user_id_sql + ")");
    conn_.exec("DELETE FROM token_channel_groups WHERE token_id IN "
               "(SELECT id FROM user_tokens WHERE user_id=" +
               user_id_sql + ")");
    conn_.exec("DELETE FROM session_bindings WHERE user_id=" + user_id_sql);
    conn_.exec("DELETE FROM user_tokens WHERE user_id=" + user_id_sql);
    if (exec_row_count(conn_, "DELETE FROM users WHERE id=" + user_id_sql) != 1) {
        return false;
    }
    tr.commit();
    return true;
}

std::optional<SessionBinding> UserStore::get_session_binding_payload(long long user_id, std::string_view route_key_hash)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    if (user_id <= 0 || route_key_hash.empty()) {
        return std::nullopt;
    }
    const std::string sql =
        "SELECT user_id,route_key_hash,payload_json,expires_at FROM session_bindings WHERE user_id=" +
        std::to_string(user_id) + " AND route_key_hash=" + conn_.quote(route_key_hash) +
        " AND expires_at > UTC_TIMESTAMP()";
    const auto rows = conn_.query_rows(sql);
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_session_binding(rows[0]);
}

void UserStore::upsert_session_binding_payload(long long user_id, std::string_view route_key_hash,
                                               std::string_view payload_json, std::string_view expires_at)
{
    assert(user_id > 0 && !route_key_hash.empty() && "internal: session binding key is invalid");
    if (user_id <= 0 || route_key_hash.empty()) {
        return;
    }
    const std::string sql = "INSERT INTO session_bindings(user_id,route_key_hash,payload_json,expires_at) VALUES(" +
                            std::to_string(user_id) + ", " + conn_.quote(route_key_hash) + ", " +
                            conn_.quote(payload_json) + ", " + conn_.quote(expires_at) +
                            ") ON DUPLICATE KEY UPDATE payload_json=VALUES(payload_json), "
                            "expires_at=VALUES(expires_at), updated_at=CURRENT_TIMESTAMP";
    conn_.exec(sql);
}

void UserStore::delete_session_binding(long long user_id, std::string_view route_key_hash)
{
    if (user_id <= 0 || route_key_hash.empty()) {
        return;
    }
    DbTransaction tr(conn_);
    conn_.exec("DELETE FROM session_bindings WHERE user_id=" + std::to_string(user_id) +
               " AND route_key_hash=" + conn_.quote(route_key_hash));
    if (conn_.affected_rows() <= 0) {
        return;
    }
    tr.commit();
}

void UserStore::delete_all_session_bindings(long long user_id)
{
    if (user_id <= 0) {
        return;
    }
    conn_.exec("DELETE FROM session_bindings WHERE user_id=" + std::to_string(user_id));
}

std::optional<User> UserStore::get_user_by_sql(const std::string &sql)
{
    const auto rows = conn_.query_rows(sql);
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_user(rows[0]);
}

WebSessionAuth web_session_auth_failure(std::string_view message, bool clear_cookie = false)
{
    WebSessionAuth result;
    result.failure_message = std::string{ message };
    result.clear_cookie = clear_cookie;
    return result;
}

std::optional<SessionCookie> verified_web_session_cookie(std::string_view raw_request, const Config &config)
{
    const auto session = cookie_value(raw_request, "revlm_session");
    if (!session.has_value()) {
        return std::nullopt;
    }
    return verify_session_cookie_value(*session, session_secret_for_config(config));
}

std::optional<long long> parse_revlm_user_header(std::string_view raw_request)
{
    return parse_positive_i64_or(header_value_from_request(raw_request, "Revlm-User"));
}

WebSessionAuth authenticate_web_session_impl(std::string_view raw_request, const Config &config,
                                             bool capture_binding_hash, bool require_root)
{
    const auto verified = verified_web_session_cookie(raw_request, config);
    if (!verified.has_value()) {
        return web_session_auth_failure("未登录", true);
    }
    const auto header_user_id = parse_revlm_user_header(raw_request);
    if (!header_user_id.has_value() || *header_user_id != verified->user_id) {
        return web_session_auth_failure("无权进行此操作，Revlm-User 无效");
    }
    try {
        const std::string hash = session_binding_hash(verified->key);
        MysqlConnection conn(config.db_dsn);
        UserStore store(conn);
        if (!store.get_session_binding_payload(verified->user_id, hash).has_value()) {
            return web_session_auth_failure("未登录", true);
        }
        auto user = store.get_user_by_id(verified->user_id);
        if (!user.has_value() || user->status != 1) {
            return web_session_auth_failure("未登录", true);
        }
        if (require_root && user->role != "root") {
            return web_session_auth_failure("无权进行此操作");
        }
        WebSessionAuth result;
        result.ok = true;
        result.user = std::move(user);
        if (capture_binding_hash) {
            result.session_binding_hash = hash;
        }
        return result;
    } catch (const std::exception &) {
        return web_session_auth_failure("未登录", true);
    }
}

WebSessionAuth authenticate_web_session(std::string_view raw_request, const Config &config, bool capture_binding_hash)
{
    return authenticate_web_session_impl(raw_request, config, capture_binding_hash, false);
}

WebSessionAuth authenticate_root_web_session(std::string_view raw_request, const Config &config)
{
    return authenticate_web_session_impl(raw_request, config, false, true);
}

} // namespace revlm
