#include "auth/session.hpp"

#include "auth/crypto.hpp"
#include "auth/security.hpp"
#include "store/database.hpp"
#include "util/datetime.hpp"
#include "util/strings.hpp"

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <cassert>
#include <chrono>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>

namespace revlm
{
namespace
{

std::unique_ptr<SessionStore> g_session_store;

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

WebSessionAuth web_session_auth_failure(std::string_view message, bool clear_cookie = false)
{
    WebSessionAuth result;
    result.failure_message = std::string{ message };
    result.clear_cookie = clear_cookie;
    return result;
}

WebSessionAuth authenticate_web_session_impl(std::string_view raw_request, bool require_root)
{
    const auto opaque = cookie_value(raw_request, session_cookie_name);
    if (!opaque.has_value() || opaque->empty()) {
        return web_session_auth_failure("未登录", true);
    }
    try {
        const std::string hash = session_token_hash(*opaque);
        SessionStore &sessions = SessionStore::instance();
        UserStore &users = UserStore::instance();
        const auto row = sessions.get_by_token_hash(hash);
        if (!row.has_value()) {
            return web_session_auth_failure("未登录", true);
        }
        User user = users.get_user_by_id(row->user_id);
        if (user.id == 0 || user.status != 1) {
            return web_session_auth_failure("未登录", true);
        }
        if (require_root && user.role != "root") {
            return web_session_auth_failure("无权进行此操作");
        }
        WebSessionAuth result;
        result.ok = true;
        result.user = std::move(user);
        result.token_hash = hash;
        return result;
    } catch (const std::exception &) {
        return web_session_auth_failure("未登录", true);
    }
}

} // namespace

SessionCookie make_session_cookie()
{
    SessionCookie session;
    session.value = base64url_encode(random_bytes(32));
    session.token_hash = session_token_hash(session.value);
    session.expires_unix = unix_now() + session_cookie_ttl_seconds;
    return session;
}

std::string session_token_hash(std::string_view opaque_token)
{
    return sha256_hex(opaque_token);
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

SessionStore &SessionStore::instance()
{
    if (!g_session_store) {
        g_session_store.reset(new SessionStore());
    }
    return *g_session_store;
}

void SessionStore::reset_instance()
{
    g_session_store.reset();
}

SessionStore::SessionStore()
    : db_(database())
{
}

SessionCookie SessionStore::create(long long user_id)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    SessionCookie session = make_session_cookie();
    if (user_id <= 0) {
        return session;
    }
    const std::string expires_at = to_mysql_datetime(unix_to_sys(static_cast<std::time_t>(session.expires_unix)));
    ScopedTransaction t(db_);
    sql_exec(db_, "INSERT INTO sessions(token_hash,user_id,expires_at) VALUES(" + sql_quote(db_, session.token_hash) +
                      ", " + std::to_string(user_id) + ", " + sql_quote(db_, expires_at) + ")");
    t.commit();
    return session;
}

std::optional<Session> SessionStore::get_by_token_hash(std::string_view token_hash)
{
    if (token_hash.empty()) {
        return std::nullopt;
    }
    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(db_, "SELECT token_hash,user_id,expires_at FROM sessions WHERE token_hash=" +
                                              sql_quote(db_, token_hash) + " AND expires_at > UTC_TIMESTAMP()");
    t.commit();
    if (rows.empty()) {
        return std::nullopt;
    }
    const auto &row = rows[0];
    if (row.size() < 3 || !row[1].has_value()) {
        return std::nullopt;
    }
    Session session;
    session.token_hash = row[0].value_or("");
    session.user_id = std::stoll(*row[1]);
    session.expires_at = row[2].value_or("");
    return session;
}

void SessionStore::delete_by_token_hash(std::string_view token_hash)
{
    if (token_hash.empty()) {
        return;
    }
    ScopedTransaction t(db_);
    sql_exec(db_, "DELETE FROM sessions WHERE token_hash=" + sql_quote(db_, token_hash));
    t.commit();
}

void SessionStore::delete_all_for_user(long long user_id)
{
    if (user_id <= 0) {
        return;
    }
    ScopedTransaction t(db_);
    sql_exec(db_, "DELETE FROM sessions WHERE user_id=" + std::to_string(user_id));
    t.commit();
}

WebSessionAuth authenticate_web_session(std::string_view raw_request)
{
    return authenticate_web_session_impl(raw_request, false);
}

WebSessionAuth authenticate_root_web_session(std::string_view raw_request)
{
    return authenticate_web_session_impl(raw_request, true);
}

} // namespace revlm
