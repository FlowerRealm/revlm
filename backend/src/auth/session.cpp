#include "auth/session.hpp"

#include "auth/crypto.hpp"
#include "auth/security.hpp"
#include "util/strings.hpp"
#include "util/user_input.hpp"

#include <cassert>
#include <chrono>
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

} // namespace

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

SessionStore::SessionStore(MysqlConnection &conn)
    : conn_(conn)
{
}

std::optional<SessionBinding> SessionStore::get_session_binding_payload(long long user_id,
                                                                        std::string_view route_key_hash)
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

void SessionStore::upsert_session_binding_payload(long long user_id, std::string_view route_key_hash,
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

void SessionStore::delete_session_binding(long long user_id, std::string_view route_key_hash)
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

void SessionStore::delete_all_session_bindings(long long user_id)
{
    if (user_id <= 0) {
        return;
    }
    conn_.exec("DELETE FROM session_bindings WHERE user_id=" + std::to_string(user_id));
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
        SessionStore sessions(conn);
        UserStore users(conn);
        if (!sessions.get_session_binding_payload(verified->user_id, hash).has_value()) {
            return web_session_auth_failure("未登录", true);
        }
        auto user = users.get_user_by_id(verified->user_id);
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
