#include "config/app_settings.hpp"
#include "channels/channel_groups.hpp"
#include "server/http_server.hpp"
#include "server/tokens.hpp"
#include "store/migrations.hpp"
#include "auth/session.hpp"
#include "auth/users.hpp"
#include "util/user_input.hpp"
#include "store/mysql_test_env.hpp"

#include <ctime>
#include <iostream>
#include <optional>

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

std::string mysql_datetime_from_unix(long long unix_seconds)
{
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return buffer;
}

} // namespace

int main()
{
    try {
        const auto env = revlm::test::prepare_mysql_test_env("app-settings");
        if (!env.has_value()) {
            return 0;
        }
        const std::string migrations_dir = "internal/store/migrations";
        const revlm::MigrationResult migrated = revlm::apply_migrations(env->dsn, migrations_dir, "", 30);
        if (expect(migrated.total >= 2, "unexpected migration count for app settings smoke") != 0) {
            return 1;
        }

        revlm::MysqlConnection conn(env->dsn);
        revlm::AppSettingsStore store(conn);
        conn.exec("DELETE FROM session_bindings");
        conn.exec("DELETE FROM users WHERE email IN ('root@example.com','user@example.com')");
        store.delete_key(revlm::setting_site_base_url);
        store.delete_key(revlm::setting_billing_paygo_price_multiplier);
        store.delete_key(revlm::setting_default_channel_group_id);

        const auto initial =
            store.get_admin_settings("GET / HTTP/1.1\r\nHost: smoke.local\r\nX-Forwarded-Proto: https\r\n\r\n");
        if (expect(initial.site_base_url_effective == "https://smoke.local",
                   "initial site_base_url effective value is wrong") != 0 ||
            expect(initial.billing_paygo_price_multiplier == 1.0, "initial multiplier default is wrong") != 0) {
            return 1;
        }

        store.update_admin_settings({
            .site_base_url = " https://admin.example.com/root/ ",
            .billing_paygo_price_multiplier = 1.234567,
        });
        const auto saved =
            store.get_admin_settings("GET / HTTP/1.1\r\nHost: saved.local\r\nX-Forwarded-Proto: https\r\n\r\n");
        const auto version1 = store.runtime_config_version().version;
        if (expect(saved.site_base_url == "https://admin.example.com/root", "saved site_base_url is wrong") != 0 ||
            expect(saved.site_base_url_override, "site_base_url override flag is wrong") != 0 ||
            expect(saved.site_base_url_effective == "https://admin.example.com/root",
                   "saved effective site_base_url is wrong") != 0 ||
            expect(saved.billing_paygo_price_multiplier == 1.234567, "saved multiplier normalization is wrong") != 0 ||
            expect(version1 > 0, "runtime_config_version should advance after update") != 0) {
            return 1;
        }

        store.update_admin_settings({
            .site_base_url = "https://admin.example.com/root",
            .billing_paygo_price_multiplier = 1.234567,
        });
        const auto version2 = store.runtime_config_version().version;
        if (expect(version2 > version1, "runtime_config_version should stay monotonic across writes") != 0) {
            return 1;
        }

        revlm::ChannelGroupStore &groups = revlm::ChannelGroupStore::instance();
        groups.reload(conn);
        const long long group_id = groups.create_channel_group("tmp-d019-default", "", 1.0);
        revlm::UserStore::instance().reload(conn);
        revlm::TokenStore &tokens = revlm::UserStore::instance().tokens();
        if (expect(tokens.set_default_channel_group_id(group_id),
                   "default_channel_group_id should persist through token store") != 0 ||
            expect(tokens.get_default_channel_group_id().value_or(0) == group_id,
                   "default_channel_group_id roundtrip is wrong") != 0) {
            return 1;
        }

        store.update_admin_settings({
            .site_base_url = "",
            .billing_paygo_price_multiplier = std::nullopt,
        });
        const auto fallback = store.get_admin_settings("GET / HTTP/1.1\r\nHost: fallback.local\r\n\r\n");
        const auto version3 = store.runtime_config_version().version;
        if (expect(!fallback.site_base_url_override, "site_base_url override should clear after delete") != 0 ||
            expect(fallback.site_base_url_effective == "http://fallback.local",
                   "request-derived site_base_url effective value is wrong") != 0 ||
            expect(fallback.billing_paygo_price_multiplier == 1.0,
                   "billing multiplier should fall back to default after delete") != 0 ||
            expect(version3 > version2, "runtime_config_version should advance after delete") != 0) {
            return 1;
        }

        const std::string session_secret = "tmp-d019-root-secret";
        revlm::UserStore &users = revlm::UserStore::instance();
        users.reload(conn);
        revlm::SessionStore sessions(conn);
        revlm::User root_id_user =
            revlm::User("root@example.com", "RootUser", revlm::hash_password("password123"), "root");
        root_id_user.status = 1;
        const long long root_id = users.create_user(std::move(root_id_user));
        const revlm::SessionCookie root_session = revlm::make_session_cookie(root_id, session_secret);
        sessions.upsert_session_binding_payload(root_id, revlm::session_binding_hash(root_session.key), "web",
                                                mysql_datetime_from_unix(root_session.expires_unix));

        revlm::Config http_config;
        http_config.db_dsn = env->dsn;
        http_config.session_secret = session_secret;
        revlm::BuildInfo build{ "test-version", "test-date" };

        store.update_admin_settings({
            .site_base_url = "",
            .billing_paygo_price_multiplier = 1.234567,
        });

        const std::string get_before =
            revlm::handle_http_request("GET /api/admin/settings HTTP/1.1\r\nHost: smoke.local\r\n"
                                       "X-Forwarded-Proto: https\r\n"
                                       "Revlm-User: " +
                                           std::to_string(root_id) +
                                           "\r\n"
                                           "Cookie: revlm_session=" +
                                           root_session.value + "\r\n\r\n",
                                       http_config, build, false, "req-admin-settings-get");
        if (expect(get_before.find("HTTP/1.1 200 OK") != std::string::npos, "admin settings GET should return 200") !=
                0 ||
            expect(get_before.find("\"success\":true") != std::string::npos,
                   "admin settings GET should report success") != 0 ||
            expect(get_before.find("\"site_base_url_effective\":\"https://smoke.local\"") != std::string::npos,
                   "admin settings GET should reflect effective request base URL") != 0 ||
            expect(get_before.find("\"billing_paygo_price_multiplier\":1.234567") != std::string::npos,
                   "admin settings GET should expose current multiplier value") != 0) {
            return 1;
        }

        const std::string put_body = "{\"site_base_url\":\"https://admin-http.example.com/base/\","
                                     "\"billing_paygo_price_multiplier\":1.75}";
        const std::string put_response =
            revlm::handle_http_request("PUT /api/admin/settings HTTP/1.1\r\nHost: smoke.local\r\n"
                                       "Content-Type: application/json\r\n"
                                       "Content-Length: " +
                                           std::to_string(put_body.size()) +
                                           "\r\n"
                                           "Revlm-User: " +
                                           std::to_string(root_id) +
                                           "\r\n"
                                           "Cookie: revlm_session=" +
                                           root_session.value + "\r\n\r\n" + put_body,
                                       http_config, build, false, "req-admin-settings-put");
        if (expect(put_response.find("HTTP/1.1 200 OK") != std::string::npos, "admin settings PUT should return 200") !=
                0 ||
            expect(put_response.find("\"success\":true") != std::string::npos,
                   "admin settings PUT should report success") != 0) {
            return 1;
        }

        const auto after_http =
            store.get_admin_settings("GET / HTTP/1.1\r\nHost: persisted.local\r\nX-Forwarded-Proto: https\r\n\r\n");
        if (expect(after_http.site_base_url == "https://admin-http.example.com/base",
                   "admin settings PUT should persist normalized site_base_url") != 0 ||
            expect(after_http.billing_paygo_price_multiplier == 1.75,
                   "admin settings PUT should persist normalized multiplier") != 0) {
            return 1;
        }

        revlm::User user("user@example.com", "NormalUser", revlm::hash_password("password123"), "user");
        user.status = 1;
        const long long user_id = users.create_user(std::move(user));
        const revlm::SessionCookie user_session = revlm::make_session_cookie(user_id, session_secret);
        sessions.upsert_session_binding_payload(user_id, revlm::session_binding_hash(user_session.key), "web",
                                                mysql_datetime_from_unix(user_session.expires_unix));
        const std::string forbidden =
            revlm::handle_http_request("GET /api/admin/settings HTTP/1.1\r\nHost: smoke.local\r\n"
                                       "Revlm-User: " +
                                           std::to_string(user_id) +
                                           "\r\n"
                                           "Cookie: revlm_session=" +
                                           user_session.value + "\r\n\r\n",
                                       http_config, build, false, "req-admin-settings-forbidden");
        if (expect(forbidden.find("\"success\":false") != std::string::npos,
                   "non-root admin settings request should fail") != 0 ||
            expect(forbidden.find("无权进行此操作") != std::string::npos,
                   "non-root admin settings request should be denied") != 0) {
            return 1;
        }

        store.delete_key(revlm::setting_site_base_url);
        store.delete_key(revlm::setting_billing_paygo_price_multiplier);
        store.delete_key(revlm::setting_default_channel_group_id);
    } catch (const std::exception &err) {
        std::cerr << "app settings mysql test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
