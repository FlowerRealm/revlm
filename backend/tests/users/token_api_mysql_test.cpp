#include "auth/session.hpp"
#include "users/users.hpp"
#include "util/user_input.hpp"
#include "channels/channel_groups.hpp"
#include "server/http_server.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"
#include "store/mysql_test_env.hpp"
#include "util/json_util.hpp"

#include <ctime>
#include <iostream>
#include <string>

namespace
{

int expect(bool ok, const char *message)
{
    if (ok) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
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

std::string request_with_session(std::string_view method, std::string_view target, std::string_view body,
                                 long long user_id, std::string_view session_value, std::string_view request_id)
{
    std::string req = std::string(method) + " " + std::string(target) +
                      " HTTP/1.1\r\nHost: smoke.local\r\nX-Forwarded-Proto: https\r\n"
                      "Revlm-User: " +
                      std::to_string(user_id) +
                      "\r\n"
                      "Cookie: revlm_session=" +
                      std::string(session_value) + "\r\n";
    if (method == "POST" || method == "PUT" || method == "PATCH") {
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    return revlm::handle_http_request(req, false, request_id);
}

std::optional<long long> parse_json_i64(std::string_view response, std::string_view key)
{
    return revlm::parse_json_int_field(response, key);
}

std::optional<std::string> parse_json_string(std::string_view response, std::string_view key)
{
    return revlm::parse_json_string_field(response, key);
}

} // namespace

int main()
{
    try {
        const auto env = revlm::test::prepare_mysql_test_env("token-api");
        if (!env.has_value()) {
            return 0;
        }

        auto db = revlm::make_database(env->dsn);
        revlm::ensure_schema(*db);
        {
            revlm::Config __runtime_cfg;
            __runtime_cfg.db_dsn = env->dsn;
            __runtime_cfg.session_secret = "tmp-token-api-secret";
            revlm::test::install_test_runtime(__runtime_cfg);
        }

        revlm::sql_exec(*db, "DELETE FROM channel_group_members");
        revlm::sql_exec(*db, "DELETE FROM channels");
        revlm::sql_exec(*db, "DELETE FROM channel_groups");
        revlm::sql_exec(*db, "DELETE FROM user_tokens");
        revlm::sql_exec(*db, "DELETE FROM session_bindings");
        revlm::sql_exec(*db, "DELETE FROM users");

        const std::string session_secret = "tmp-token-api-secret";
        revlm::UserStore &users = revlm::UserStore::instance();
        revlm::SessionStore &sessions = revlm::SessionStore::instance();
        revlm::User user("token-api@example.com", "tokenapi", revlm::hash_password("password123"), "user");
        user.status = 1;
        const long long user_id = users.create_user(std::move(user));
        const revlm::SessionCookie session = revlm::make_session_cookie(user_id, session_secret);
        sessions.upsert_session_binding_payload(user_id, revlm::session_binding_hash(session.key), "web",
                                                mysql_datetime_from_unix(session.expires_unix));

        const std::string unauth_list = revlm::handle_http_request(
            "GET /api/token HTTP/1.1\r\nHost: smoke.local\r\n\r\n", false, "req-token-unauth");
        if (expect(contains(unauth_list, "HTTP/1.1 200 OK"), "unauthenticated token list should return 200") != 0 ||
            expect(contains(unauth_list, "\"success\":false"), "unauthenticated token list should fail") != 0 ||
            expect(contains(unauth_list, "未登录"), "unauthenticated token list should say 未登录") != 0) {
            std::cerr << unauth_list << '\n';
            return 1;
        }

        const std::string create_body = R"({"name":"tmp-main"})";
        const std::string create_resp =
            request_with_session("POST", "/api/token", create_body, user_id, session.value, "req-token-create");
        const auto token_id = parse_json_i64(create_resp, "token_id");
        const auto created_token = parse_json_string(create_resp, "token");
        if (expect(contains(create_resp, "HTTP/1.1 200 OK"), "token create should return 200") != 0 ||
            expect(contains(create_resp, "\"success\":true"), "token create should succeed") != 0 ||
            expect(token_id.has_value() && *token_id > 0, "token create should return token_id") != 0 ||
            expect(created_token.has_value() && created_token->rfind("sk_", 0) == 0,
                   "token create should return sk_ token") != 0) {
            std::cerr << create_resp << '\n';
            return 1;
        }

        const std::string list_resp =
            request_with_session("GET", "/api/token", "", user_id, session.value, "req-token-list");
        if (expect(contains(list_resp, "\"success\":true"), "token list should succeed") != 0 ||
            expect(contains(list_resp, "\"name\":\"tmp-main\""), "token list should include name") != 0 ||
            expect(contains(list_resp, "\"status\":1"), "token list should show active status") != 0) {
            std::cerr << list_resp << '\n';
            return 1;
        }

        const std::string reveal_path = "/api/token/" + std::to_string(*token_id) + "/reveal";
        const std::string reveal_resp =
            request_with_session("GET", reveal_path, "", user_id, session.value, "req-token-reveal");
        if (expect(contains(reveal_resp, "\"success\":true"), "token reveal should succeed") != 0 ||
            expect(contains(reveal_resp, "\"token\":\"" + *created_token + "\""),
                   "token reveal should return stored token") != 0) {
            std::cerr << reveal_resp << '\n';
            return 1;
        }

        const std::string rotate_path = "/api/token/" + std::to_string(*token_id) + "/rotate";
        const std::string rotate_resp =
            request_with_session("POST", rotate_path, "", user_id, session.value, "req-token-rotate");
        const auto rotated_token = parse_json_string(rotate_resp, "token");
        if (expect(contains(rotate_resp, "\"success\":true"), "token rotate should succeed") != 0 ||
            expect(rotated_token.has_value() && *rotated_token != *created_token,
                   "token rotate should change token value") != 0) {
            std::cerr << rotate_resp << '\n';
            return 1;
        }

        revlm::ChannelGroupStore &group_store = revlm::ChannelGroupStore::instance();
        const int group_id = group_store.create_channel_group("tmp-a003-openai", "", 1.0, true);
        if (group_id <= 0) {
            std::cerr << "failed to create channel group\n";
            return 1;
        }

        const std::string channel_path = "/api/token/" + std::to_string(*token_id) + "/channel";
        const std::string bind_channel_body = "{\"channel_group_id\":" + std::to_string(group_id) + "}";
        const std::string bind_channel_resp = request_with_session("PUT", channel_path, bind_channel_body, user_id,
                                                                   session.value, "req-token-bind-channel");
        if (expect(contains(bind_channel_resp, "\"success\":true"), "token channel group replace should succeed") !=
            0) {
            std::cerr << bind_channel_resp << '\n';
            return 1;
        }

        const std::string channel_get_resp =
            request_with_session("GET", channel_path, "", user_id, session.value, "req-token-get-channel");
        if (expect(contains(channel_get_resp, "\"channel_group_id\":" + std::to_string(group_id)),
                   "token channel GET should include binding") != 0 ||
            expect(contains(channel_get_resp, "\"allowed_channel_groups\""),
                   "token channel GET should include allowed channel groups") != 0 ||
            expect(contains(channel_get_resp, "\"name\":\"tmp-a003-openai\""),
                   "token channel GET should include channel group name") != 0) {
            std::cerr << channel_get_resp << '\n';
            return 1;
        }

        const std::string revoke_path = "/api/token/" + std::to_string(*token_id) + "/revoke";
        const std::string revoke_resp =
            request_with_session("POST", revoke_path, "", user_id, session.value, "req-token-revoke");
        if (expect(contains(revoke_resp, "\"success\":true"), "token revoke should succeed") != 0) {
            std::cerr << revoke_resp << '\n';
            return 1;
        }

        const std::string reveal_revoked_resp =
            request_with_session("GET", reveal_path, "", user_id, session.value, "req-token-reveal-revoked");
        if (expect(contains(reveal_revoked_resp, "\"success\":false"), "reveal on revoked token should fail") != 0 ||
            expect(contains(reveal_revoked_resp, "令牌不存在"), "reveal on revoked token should say 令牌不存在") != 0) {
            std::cerr << reveal_revoked_resp << '\n';
            return 1;
        }

        const std::string create_second =
            request_with_session("POST", "/api/token", "{}", user_id, session.value, "req-token-create-2");
        const auto second_id = parse_json_i64(create_second, "token_id");
        if (!second_id.has_value()) {
            std::cerr << create_second << '\n';
            return 1;
        }

        const std::string delete_path = "/api/token/" + std::to_string(*second_id);
        const std::string delete_resp =
            request_with_session("DELETE", delete_path, "", user_id, session.value, "req-token-delete");
        if (expect(contains(delete_resp, "\"success\":true"), "token delete should succeed") != 0) {
            std::cerr << delete_resp << '\n';
            return 1;
        }

        const std::string delete_missing_resp =
            request_with_session("DELETE", delete_path, "", user_id, session.value, "req-token-delete-missing");
        if (expect(contains(delete_missing_resp, "\"success\":false"), "delete missing token should fail") != 0 ||
            expect(contains(delete_missing_resp, "令牌不存在"), "delete missing token should say 令牌不存在") != 0) {
            std::cerr << delete_missing_resp << '\n';
            return 1;
        }

        const std::string bad_id_resp =
            request_with_session("GET", "/api/token/0/reveal", "", user_id, session.value, "req-token-bad-id");
        if (expect(contains(bad_id_resp, "\"success\":false"), "invalid token_id should fail") != 0 ||
            expect(contains(bad_id_resp, "token_id 不合法"), "invalid token_id should explain failure") != 0) {
            std::cerr << bad_id_resp << '\n';
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "token api mysql test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
