#include "users/users.hpp"
#include "server/http_server.hpp"
#include "store/database.hpp"
#include "store/mysql_test_env.hpp"
#include "util/json_util.hpp"

#include <iostream>
#include <optional>
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

std::string handle(std::string_view method, std::string_view target, std::string_view body, std::string_view request_id,
                   long long user_id = 0, std::string_view session_value = {})
{
    std::string req = std::string(method) + " " + std::string(target) +
                      " HTTP/1.1\r\nHost: smoke.local\r\nX-Forwarded-Proto: https\r\n";
    if (user_id > 0 && !session_value.empty()) {
        req += "Revlm-User: " + std::to_string(user_id) + "\r\n";
        req += "Cookie: revlm_session=" + std::string(session_value) + "\r\n";
    }
    if (!body.empty()) {
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    return revlm::handle_http_request(req, false, request_id);
}

std::optional<std::string> parse_set_cookie_session(std::string_view response)
{
    const std::string marker = "Set-Cookie: revlm_session=";
    const size_t pos = response.find(marker);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t start = pos + marker.size();
    const size_t end = response.find(';', start);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return std::string{ response.substr(start, end - start) };
}

std::optional<long long> parse_json_i64(std::string_view response, std::string_view key)
{
    return revlm::parse_json_int_field(response, key);
}

} // namespace

int main()
{
    try {
        const auto env = revlm::test::prepare_mysql_test_env("user-api");
        if (!env.has_value()) {
            return 0;
        }

        auto db = revlm::make_database(env->dsn);
        revlm::ensure_schema(*db);

        revlm::sql_exec(*db, "DELETE FROM session_bindings");
        revlm::sql_exec(*db, "DELETE FROM users");

        revlm::Config config;
        config.db_dsn = env->dsn;
        config.session_secret = "tmp-a002-secret";
        revlm::test::install_test_runtime(config);

        const std::string register_body =
            R"({"email":"alice@example.com","username":"alice","password":"password123"})";
        const std::string register_resp = handle("POST", "/api/user/register", register_body, "req-register");
        if (expect(contains(register_resp, "\"success\":true"), "register should succeed") != 0 ||
            expect(contains(register_resp, "\"role\":\"root\""), "first user should be root") != 0) {
            std::cerr << register_resp << '\n';
            return 1;
        }
        const auto register_user_id = parse_json_i64(register_resp, "id");
        const auto register_session = parse_set_cookie_session(register_resp);
        if (expect(register_user_id.has_value() && register_session.has_value(),
                   "register should set session cookie and user id") != 0) {
            std::cerr << register_resp << '\n';
            return 1;
        }

        const std::string self_after_register =
            handle("GET", "/api/user/self", "", "req-self-register", *register_user_id, *register_session);
        if (expect(contains(self_after_register, "\"success\":true"), "self after register should succeed") != 0 ||
            expect(contains(self_after_register, "\"email\":\"alice@example.com\""),
                   "self should return registered email") != 0) {
            std::cerr << self_after_register << '\n';
            return 1;
        }

        const std::string logout_resp =
            handle("GET", "/api/user/logout", "", "req-logout", *register_user_id, *register_session);
        if (expect(contains(logout_resp, "\"success\":true"), "logout should succeed") != 0) {
            std::cerr << logout_resp << '\n';
            return 1;
        }

        const std::string login_body = R"({"login":"alice","password":"password123"})";
        const std::string login_resp = handle("POST", "/api/user/login", login_body, "req-login");
        if (expect(contains(login_resp, "\"success\":true"), "login should succeed") != 0) {
            std::cerr << login_resp << '\n';
            return 1;
        }
        const auto login_user_id = parse_json_i64(login_resp, "id");
        const auto login_session = parse_set_cookie_session(login_resp);
        if (expect(login_user_id.has_value() && login_session.has_value(),
                   "login should set session cookie and user id") != 0) {
            std::cerr << login_resp << '\n';
            return 1;
        }

        const std::string email_body = R"({"email":"alice2@example.com","current_password":"password123"})";
        const std::string email_resp =
            handle("POST", "/api/account/email", email_body, "req-email", *login_user_id, *login_session);
        if (expect(contains(email_resp, "\"success\":true"), "account email update should succeed") != 0 ||
            expect(contains(email_resp, "\"force_logout\":true"), "account email update should force logout") != 0) {
            std::cerr << email_resp << '\n';
            return 1;
        }

        const std::string stale_self =
            handle("GET", "/api/user/self", "", "req-stale-self", *login_user_id, *login_session);
        if (expect(contains(stale_self, "\"success\":false"), "self after forced logout should fail") != 0) {
            std::cerr << stale_self << '\n';
            return 1;
        }

        const std::string relogin_body = R"({"login":"alice2@example.com","password":"password123"})";
        const std::string relogin_resp = handle("POST", "/api/user/login", relogin_body, "req-relogin");
        if (expect(contains(relogin_resp, "\"success\":true"), "relogin after email change") != 0) {
            std::cerr << relogin_resp << '\n';
            return 1;
        }
        const auto relogin_user_id = parse_json_i64(relogin_resp, "id");
        const auto relogin_session = parse_set_cookie_session(relogin_resp);
        if (!relogin_user_id.has_value() || !relogin_session.has_value()) {
            std::cerr << relogin_resp << '\n';
            return 1;
        }

        const std::string bad_password_body = R"({"old_password":"wrong-password","new_password":"newpassword456"})";
        const std::string bad_password_resp = handle("POST", "/api/account/password", bad_password_body,
                                                     "req-bad-password", *relogin_user_id, *relogin_session);
        if (expect(contains(bad_password_resp, "\"success\":false"),
                   "wrong old password should fail password change") != 0 ||
            expect(contains(bad_password_resp, "旧密码错误"), "wrong old password should return old password error") !=
                0) {
            std::cerr << bad_password_resp << '\n';
            return 1;
        }

        const std::string password_body = R"({"old_password":"password123","new_password":"newpassword456"})";
        const std::string password_resp =
            handle("POST", "/api/account/password", password_body, "req-password", *relogin_user_id, *relogin_session);
        if (expect(contains(password_resp, "\"success\":true"), "account password update should succeed") != 0 ||
            expect(contains(password_resp, "\"force_logout\":true"), "account password update should force logout") !=
                0) {
            std::cerr << password_resp << '\n';
            return 1;
        }

        const std::string bad_login = handle(
            "POST", "/api/user/login", R"({"login":"alice2@example.com","password":"password123"})", "req-bad-login");
        if (expect(contains(bad_login, "\"success\":false"), "old password should fail after password change") != 0) {
            std::cerr << bad_login << '\n';
            return 1;
        }

        const std::string good_login = handle("POST", "/api/user/login",
                                              R"({"login":"alice2@example.com","password":"newpassword456"})",
                                              "req-good-login");
        if (expect(contains(good_login, "\"success\":true"), "login with new password should succeed") != 0) {
            std::cerr << good_login << '\n';
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "user api mysql test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
