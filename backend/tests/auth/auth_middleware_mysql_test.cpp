#include "auth/users.hpp"
#include "server/http_server.hpp"
#include "store/migrations.hpp"
#include "store/mysql_test_env.hpp"

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

std::string request_with_session(std::string_view method, std::string_view target, long long user_id,
                                 std::string_view session_value)
{
    return std::string(method) + " " + std::string(target) +
           " HTTP/1.1\r\nHost: smoke.local\r\n"
           "Revlm-User: " +
           std::to_string(user_id) +
           "\r\n"
           "Cookie: revlm_session=" +
           std::string(session_value) + "\r\n\r\n";
}

} // namespace

int main()
{
    try {
        const auto env = revlm::test::prepare_mysql_test_env("web-session");
        if (!env.has_value()) {
            return 0;
        }

        (void)revlm::apply_migrations(env->dsn, "internal/store/migrations", "", 30);

        revlm::MysqlConnection conn(env->dsn);
        conn.exec("DELETE FROM session_bindings");
        conn.exec("DELETE FROM users");

        const std::string session_secret = "tmp-a001-secret";
        revlm::UserStore users(conn);
        const long long root_id =
            users.create_user({ "root@example.com", "root", revlm::hash_password("password123"), "root" });
        const long long user_id =
            users.create_user({ "user@example.com", "user", revlm::hash_password("password123"), "user" });

        const revlm::SessionCookie root_session = revlm::make_session_cookie(root_id, session_secret);
        users.upsert_session_binding_payload(root_id, revlm::session_binding_hash(root_session.key), "web",
                                             mysql_datetime_from_unix(root_session.expires_unix));

        const revlm::SessionCookie user_session = revlm::make_session_cookie(user_id, session_secret);
        users.upsert_session_binding_payload(user_id, revlm::session_binding_hash(user_session.key), "web",
                                             mysql_datetime_from_unix(user_session.expires_unix));

        revlm::Config config;
        config.role = revlm::RuntimeRole::Api;
        config.db_dsn = env->dsn;
        config.session_secret = session_secret;
        revlm::BuildInfo build{ "test-version", "test-date" };

        const std::string self =
            revlm::handle_http_request(request_with_session("GET", "/api/user/self", user_id, user_session.value),
                                       config, build, false, "req-self");
        if (expect(contains(self, "\"success\":true"), "session user self should succeed") != 0 ||
            expect(contains(self, "\"email\":\"user@example.com\""), "self should return user email") != 0) {
            std::cerr << self << '\n';
            return 1;
        }

        const std::string admin_session =
            revlm::handle_http_request(request_with_session("GET", "/api/admin/settings", root_id, root_session.value),
                                       config, build, false, "req-admin-session");
        if (expect(contains(admin_session, "\"success\":true"), "root session admin should succeed") != 0) {
            std::cerr << admin_session << '\n';
            return 1;
        }

        const std::string forbidden =
            revlm::handle_http_request(request_with_session("GET", "/api/admin/settings", user_id, user_session.value),
                                       config, build, false, "req-forbidden");
        if (expect(contains(forbidden, "\"success\":false"), "non-root admin should fail") != 0 ||
            expect(contains(forbidden, "无权进行此操作"), "non-root admin denial message") != 0) {
            std::cerr << forbidden << '\n';
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "web session mysql test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
