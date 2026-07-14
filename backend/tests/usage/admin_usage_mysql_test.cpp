#include "server/http_server.hpp"
#include "auth/session.hpp"
#include "auth/users.hpp"
#include "util/user_input.hpp"
#include "store/database.hpp"
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

std::string root_request(std::string_view method, std::string_view target, long long root_id,
                         std::string_view session_value)
{
    return std::string(method) + " " + std::string(target) +
           " HTTP/1.1\r\nHost: smoke.local\r\n"
           "Revlm-User: " +
           std::to_string(root_id) +
           "\r\n"
           "Cookie: revlm_session=" +
           std::string(session_value) + "\r\n\r\n";
}

} // namespace

int main()
{
    try {
        const auto env = revlm::test::prepare_mysql_test_env("admin-usage");
        if (!env.has_value()) {
            return 0;
        }

        auto db = revlm::make_database(env->dsn);
        revlm::ensure_schema(*db);

        revlm::sql_exec(*db, "DELETE FROM session_bindings");
        revlm::sql_exec(*db, "DELETE FROM users");

        const std::string session_secret = "tmp-a008-secret";
        revlm::UserStore users(*db);
        revlm::SessionStore sessions(*db);
        revlm::User root_id_user = revlm::User("root@example.com", "root", revlm::hash_password("password123"), "root");
        root_id_user.status = 1;
        const long long root_id = users.create_user(std::move(root_id_user));

        const revlm::SessionCookie root_session = revlm::make_session_cookie(root_id, session_secret);
        sessions.upsert_session_binding_payload(root_id, revlm::session_binding_hash(root_session.key), "web",
                                              mysql_datetime_from_unix(root_session.expires_unix));

        revlm::Config config;
        config.db_dsn = env->dsn;
        config.session_secret = session_secret;
        revlm::BuildInfo build{ "test-version", "test-date" };

        const std::string dashboard =
            revlm::handle_http_request(root_request("GET", "/api/admin/dashboard", root_id, root_session.value), config,
                                       build, false, "req-dashboard");
        if (expect(contains(dashboard, "\"success\":true"), "admin dashboard should succeed") != 0 ||
            expect(contains(dashboard, "\"admin_time_zone\":\"Asia/Shanghai\""), "dashboard timezone") != 0 ||
            expect(contains(dashboard, "\"users_count\":"), "dashboard users_count") != 0) {
            std::cerr << dashboard << '\n';
            return 1;
        }

        const std::string usage = revlm::handle_http_request(
            root_request("GET", "/api/admin/request", root_id, root_session.value), config, build, false, "req-usage");
        if (expect(contains(usage, "\"success\":true"), "admin usage page should succeed") != 0 ||
            expect(contains(usage, "\"events\":"), "usage events array") != 0 ||
            expect(contains(usage, "\"admin_time_zone\":\"Asia/Shanghai\""), "usage timezone") != 0) {
            std::cerr << usage << '\n';
            return 1;
        }

        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "admin usage mysql test failed: " << ex.what() << '\n';
        return 1;
    }
}
