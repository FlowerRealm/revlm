#include "server/http_server.hpp"
#include "store/migrations.hpp"
#include "store/mysql.hpp"
#include "auth/session.hpp"
#include "auth/users.hpp"
#include "store/mysql_test_env.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <future>
#include <string>
#include <string_view>

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

std::string body_of(std::string_view response)
{
    const size_t pos = response.find("\r\n\r\n");
    if (pos == std::string::npos) {
        return {};
    }
    return std::string{ response.substr(pos + 4) };
}

std::string cookie_pair(std::string_view set_cookie)
{
    const size_t semi = set_cookie.find(';');
    return std::string{ set_cookie.substr(0, semi == std::string_view::npos ? set_cookie.size() : semi) };
}

std::string request_with_body(std::string_view method, std::string_view path, std::string_view body,
                              std::string_view user_id, std::string_view cookie)
{
    std::string request = std::string{ method } + " " + std::string{ path } +
                          " HTTP/1.1\r\n"
                          "Host: test\r\n"
                          "Content-Type: application/json\r\n"
                          "Revlm-User: " +
                          std::string{ user_id } +
                          "\r\n"
                          "Cookie: " +
                          std::string{ cookie } +
                          "\r\n"
                          "Content-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + std::string{ body };
    return request;
}

} // namespace

int main()
{
    try {
        const auto env = revlm::test::prepare_mysql_test_env("admin-users");
        if (!env.has_value()) {
            return 0;
        }
        const std::string &dsn = env->dsn;

        revlm::apply_migrations(dsn, "internal/store/migrations", "tmp.revlm.a005.admin_users_test", 5);
        revlm::MysqlConnection conn(dsn);
        conn.exec("DELETE FROM session_bindings");
        conn.exec("DELETE FROM token_model_mappings");
        conn.exec("DELETE FROM token_channel_groups");
        conn.exec("DELETE FROM user_tokens");
        conn.exec("DELETE FROM usage_events");
        conn.exec("DELETE FROM user_balances");
        conn.exec("DELETE FROM users");

        revlm::UserStore store(conn);
        revlm::SessionStore sessions(conn);
        const long long root_id =
            store.create_user(revlm::User("root@example.com", "root", revlm::hash_password("root-pass-123"), "root"));
        const revlm::SessionCookie root_session = revlm::make_session_cookie(root_id, "test-secret");
        sessions.upsert_session_binding_payload(root_id, revlm::session_binding_hash(root_session.key), "web",
                                              "2099-01-01 00:00:00");

        revlm::Config config;
        config.db_dsn = dsn;
        config.session_secret = "test-secret";
        revlm::BuildInfo build{ "test-version", "test-date" };

        const std::string cookie = cookie_pair("revlm_session=" + root_session.value + "; Path=/");

        const std::string create_res = revlm::handle_http_request(
            request_with_body(
                "POST", "/api/admin/users",
                R"({"email":"alice@example.com","username":"Alice09","password":"password123","role":"user"})",
                std::to_string(root_id), cookie),
            config, build, false, "req-create");
        const std::string create_body = body_of(create_res);
        if (expect(create_res.find("HTTP/1.1 200 OK") != std::string::npos, "create should return 200") != 0 ||
            expect(create_body.find("\"success\":true") != std::string::npos, "create should succeed") != 0) {
            return 1;
        }

        const auto created = store.get_user_by_email("alice@example.com");
        if (expect(created.has_value(), "created user should exist") != 0) {
            return 1;
        }

        const std::string update_res =
            revlm::handle_http_request(request_with_body("PUT", "/api/admin/users/" + std::to_string(created->id),
                                                         R"({"email":"alice2@example.com","status":1,"role":"root"})",
                                                         std::to_string(root_id), cookie),
                                       config, build, false, "req-update");
        if (expect(body_of(update_res).find("\"success\":true") != std::string::npos, "update should succeed") != 0) {
            return 1;
        }

        const std::string bogus_delete_res =
            revlm::handle_http_request("DELETE /api/admin/users/" + std::to_string(created->id) +
                                           "/password HTTP/1.1\r\nHost: test\r\nRevlm-User: " +
                                           std::to_string(root_id) + "\r\nCookie: " + cookie + "\r\n\r\n",
                                       config, build, false, "req-bogus-delete");
        if (expect(bogus_delete_res.find("HTTP/1.1 404 Not Found") != std::string::npos,
                   "delete on subpath should not match item route") != 0 ||
            expect(store.get_user_by_id(created->id).has_value(), "bogus subpath delete must not remove user") != 0) {
            return 1;
        }

        const std::string balance_res = revlm::handle_http_request(
            request_with_body("POST", "/api/admin/users/" + std::to_string(created->id) + "/balance",
                              R"({"amount_usd":"12.5"})", std::to_string(root_id), cookie),
            config, build, false, "req-balance");
        if (expect(body_of(balance_res).find("\"balance_usd\":\"12.5\"") != std::string::npos,
                   "balance should update") != 0) {
            return 1;
        }

        const std::string reset_res = revlm::handle_http_request(
            request_with_body("POST", "/api/admin/users/" + std::to_string(created->id) + "/password",
                              R"({"password":"new-password123"})", std::to_string(root_id), cookie),
            config, build, false, "req-password");
        if (expect(body_of(reset_res).find("\"success\":true") != std::string::npos, "password reset should succeed") !=
            0) {
            return 1;
        }

        const std::string missing_password_res = revlm::handle_http_request(
            request_with_body("POST", "/api/admin/users/999999/password", R"({"password":"new-password123"})",
                              std::to_string(root_id), cookie),
            config, build, false, "req-missing-password");
        const std::string missing_balance_res =
            revlm::handle_http_request(request_with_body("POST", "/api/admin/users/999999/balance",
                                                         R"({"amount_usd":"1.5"})", std::to_string(root_id), cookie),
                                       config, build, false, "req-missing-balance");
        const std::string missing_delete_res =
            revlm::handle_http_request("DELETE /api/admin/users/999999 HTTP/1.1\r\nHost: test\r\nRevlm-User: " +
                                           std::to_string(root_id) + "\r\nCookie: " + cookie + "\r\n\r\n",
                                       config, build, false, "req-missing-delete");
        if (expect(body_of(missing_password_res).find("用户不存在") != std::string::npos,
                   "missing password target should fail explicitly") != 0 ||
            expect(body_of(missing_balance_res).find("用户不存在") != std::string::npos,
                   "missing balance target should fail explicitly") != 0 ||
            expect(body_of(missing_delete_res).find("用户不存在") != std::string::npos,
                   "missing delete target should fail explicitly") != 0 ||
            expect(conn.query_one("SELECT COUNT(*) FROM user_balances WHERE user_id=999999").value_or("0") == "0",
                   "missing balance target must not create orphan balance rows") != 0) {
            return 1;
        }

        const long long race_user_id =
            store.create_user(revlm::User("race@example.com", "raceuser9", revlm::hash_password("race-pass-123"),
                                          "user"));
        revlm::MysqlConnection lock_conn(dsn);
        revlm::DbTransaction tr(lock_conn);
        if (expect(lock_conn.query_one("SELECT id FROM users WHERE id=" + std::to_string(race_user_id) + " FOR UPDATE")
                       .has_value(),
                   "race user lock should exist") != 0) {
            return 1;
        }
        auto race_balance = std::async(std::launch::async, [&] {
            return revlm::handle_http_request(
                request_with_body("POST", "/api/admin/users/" + std::to_string(race_user_id) + "/balance",
                                  R"({"amount_usd":"2.0"})", std::to_string(root_id), cookie),
                config, build, false, "req-race-balance");
        });
        auto race_password = std::async(std::launch::async, [&] {
            return revlm::handle_http_request(
                request_with_body("POST", "/api/admin/users/" + std::to_string(race_user_id) + "/password",
                                  R"({"password":"race-new-pass"})", std::to_string(root_id), cookie),
                config, build, false, "req-race-password");
        });
        auto race_delete = std::async(std::launch::async, [&] {
            return revlm::handle_http_request("DELETE /api/admin/users/" + std::to_string(race_user_id) +
                                                  " HTTP/1.1\r\nHost: test\r\nRevlm-User: " + std::to_string(root_id) +
                                                  "\r\nCookie: " + cookie + "\r\n\r\n",
                                              config, build, false, "req-race-delete");
        });
        if (expect(race_balance.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
                   "race balance request should block on row lock") != 0 ||
            expect(race_password.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
                   "race password request should block on row lock") != 0 ||
            expect(race_delete.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
                   "race delete request should block on row lock") != 0) {
            return 1;
        }
        lock_conn.exec("DELETE FROM users WHERE id=" + std::to_string(race_user_id));
        tr.commit();
        const std::string race_balance_res = race_balance.get();
        const std::string race_password_res = race_password.get();
        const std::string race_delete_res = race_delete.get();
        if (expect(body_of(race_balance_res).find("用户不存在") != std::string::npos,
                   "race balance target should fail after concurrent delete") != 0 ||
            expect(body_of(race_password_res).find("用户不存在") != std::string::npos,
                   "race password target should fail after concurrent delete") != 0 ||
            expect(body_of(race_delete_res).find("用户不存在") != std::string::npos,
                   "race delete target should fail after concurrent delete") != 0 ||
            expect(conn.query_one("SELECT COUNT(*) FROM user_balances WHERE user_id=" + std::to_string(race_user_id))
                           .value_or("0") == "0",
                   "race balance path must not create orphan balance rows") != 0 ||
            expect(!store.get_user_by_id(race_user_id).has_value(), "race user should remain deleted") != 0) {
            return 1;
        }

        const std::string list_res =
            revlm::handle_http_request("GET /api/admin/users HTTP/1.1\r\nHost: test\r\nRevlm-User: " +
                                           std::to_string(root_id) + "\r\nCookie: " + cookie + "\r\n\r\n",
                                       config, build, false, "req-list");
        const std::string list_body = body_of(list_res);
        if (expect(list_body.find("\"email\":\"alice2@example.com\"") != std::string::npos,
                   "list should show updated email") != 0 ||
            expect(list_body.find("\"balance_usd\":\"12.5\"") != std::string::npos,
                   "list should show updated balance") != 0 ||
            expect(list_body.find("\"role\":\"root\"") != std::string::npos, "list should show updated role") != 0) {
            return 1;
        }

        const std::string self_delete_res =
            revlm::handle_http_request("DELETE /api/admin/users/" + std::to_string(root_id) +
                                           " HTTP/1.1\r\nHost: test\r\nRevlm-User: " + std::to_string(root_id) +
                                           "\r\nCookie: " + cookie + "\r\n\r\n",
                                       config, build, false, "req-self-delete");
        if (expect(body_of(self_delete_res).find("不能删除当前登录用户") != std::string::npos,
                   "self delete should be rejected") != 0) {
            return 1;
        }

        const std::string delete_res =
            revlm::handle_http_request("DELETE /api/admin/users/" + std::to_string(created->id) +
                                           " HTTP/1.1\r\nHost: test\r\nRevlm-User: " + std::to_string(root_id) +
                                           "\r\nCookie: " + cookie + "\r\n\r\n",
                                       config, build, false, "req-delete");
        if (expect(body_of(delete_res).find("\"success\":true") != std::string::npos, "delete should succeed") != 0 ||
            expect(!store.get_user_by_id(created->id).has_value(), "deleted user should be gone") != 0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << err.what() << '\n';
        return 1;
    }

    return 0;
}
