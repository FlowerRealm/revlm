#include "auth/session.hpp"
#include "config/config.hpp"
#include "users/users.hpp"
#include "util/user_input.hpp"
#include "server/http_server.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"
#include "store/mysql_test_env.hpp"

#include <ctime>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

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

std::string request_with_session(std::string_view method, std::string_view target, long long /*user_id*/,
                                 std::string_view session_value)
{
    return std::string(method) + " " + std::string(target) +
           " HTTP/1.1\r\nHost: smoke.local\r\nCookie: revlm_session=" + std::string(session_value) + "\r\n\r\n";
}

} // namespace

int main()
{
    try {
        const auto env = revlm::test::prepare_mysql_test_env("web-session");
        if (!env.has_value()) {
            return 0;
        }

        auto db = revlm::make_database(env->dsn);
        revlm::ensure_schema(*db);
        {
            revlm::Config __runtime_cfg;
            __runtime_cfg.db_dsn = env->dsn;
            revlm::test::install_test_runtime(__runtime_cfg);
        }

        revlm::sql_exec(*db, "DELETE FROM sessions");
        revlm::sql_exec(*db, "DELETE FROM users");
        revlm::UserStore &users = revlm::UserStore::instance();
        revlm::SessionStore &sessions = revlm::SessionStore::instance();
        revlm::User root_id_user = revlm::User("root@example.com", "root", revlm::hash_password("password123"), "root");
        root_id_user.status = 1;
        const long long root_id = users.create_user(std::move(root_id_user));
        revlm::User user_id_user = revlm::User("user@example.com", "user", revlm::hash_password("password123"), "user");
        user_id_user.status = 1;
        const long long user_id = users.create_user(std::move(user_id_user));

        const revlm::SessionCookie root_session = sessions.create(root_id);

        const revlm::SessionCookie user_session = sessions.create(user_id);

        const std::string self = revlm::handle_http_request(
            request_with_session("GET", "/api/user/self", user_id, user_session.value), false);
        if (expect(contains(self, "\"success\":true"), "session user self should succeed") != 0 ||
            expect(contains(self, "\"email\":\"user@example.com\""), "self should return user email") != 0) {
            std::cerr << self << '\n';
            return 1;
        }

        const std::string admin_session = revlm::handle_http_request(
            request_with_session("GET", "/api/admin/dashboard", root_id, root_session.value), false);
        if (expect(contains(admin_session, "\"success\":true"), "root session admin should succeed") != 0) {
            std::cerr << admin_session << '\n';
            return 1;
        }

        const std::string forbidden = revlm::handle_http_request(
            request_with_session("GET", "/api/admin/dashboard", user_id, user_session.value), false);
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
