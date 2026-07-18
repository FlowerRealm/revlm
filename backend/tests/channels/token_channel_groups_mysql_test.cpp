#include "channels/channel_groups.hpp"
#include "store/mysql_test_env.hpp"
#include "server/http_server.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"
#include "users/tokens.hpp"
#include "auth/session.hpp"
#include "users/users.hpp"
#include "util/user_input.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
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

int expect_contains(const std::string &haystack, std::string_view needle, const char *message)
{
    if (haystack.find(needle) != std::string::npos) {
        return 0;
    }
    std::cerr << message << '\n' << haystack << '\n';
    return 1;
}

std::string make_api_request(std::string_view method, std::string_view target, long long user_id,
                             std::string_view session_cookie, std::string_view body = {})
{
    std::string request = std::string(method) + " " + std::string(target) +
                          " HTTP/1.1\r\n"
                          "Host: test\r\n"
                          "Revlm-User: " +
                          std::to_string(user_id) +
                          "\r\n"
                          "Cookie: revlm_session=" +
                          std::string(session_cookie) + "\r\n";
    if (!body.empty()) {
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    request.append(body.data(), body.size());
    return request;
}

} // namespace

int main()
{
    const char *dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping token channel group contract test\n";
        return 0;
    }

    try {
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);
        {
            revlm::Config __runtime_cfg;
            __runtime_cfg.db_dsn = dsn;
            __runtime_cfg.session_secret = "tmp-a003-contract-secret";
            revlm::test::install_test_runtime(__runtime_cfg);
        }

        revlm::UserStore &users = revlm::UserStore::instance();
        revlm::SessionStore &sessions = revlm::SessionStore::instance();
        revlm::ChannelGroupStore &groups = revlm::ChannelGroupStore::instance();
        revlm::TokenStore &tokens = users.tokens();

        const auto now =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch());
        const std::string suffix = std::to_string(now.count());
        const std::string username = "tmpa003" + suffix;
        const std::string email = username + "@example.com";

        revlm::User user_id_user = revlm::User(email, username, revlm::hash_password("password123"), "user");
        user_id_user.status = 1;
        const long long user_id = users.create_user(std::move(user_id_user));

        const std::string enabled_name = "tmp-enabled-" + suffix;
        const int enabled_group_id = groups.create_channel_group(enabled_name, "", 1.25, true);
        const int alt_group_id = groups.create_channel_group("tmp-alt-" + suffix, "", 1.0, true);
        const int disabled_group_id = groups.create_channel_group("tmp-disabled-" + suffix, "", 1.0, false);
        if (expect(enabled_group_id > 0 && alt_group_id > 0 && disabled_group_id > 0,
                   "channel groups should be created") != 0) {
            return 1;
        }

        const long long token_id = tokens.create_user_token(user_id, odb::nullable<std::string>{ "tmp contract token" },
                                                            "sk_tmp_contract_" + suffix);
        if (expect(tokens.set_token_channel_group(user_id, token_id, enabled_group_id),
                   "initial token channel group bind should succeed") != 0) {
            return 1;
        }

        const revlm::SessionCookie session = revlm::make_session_cookie(user_id, revlm::session_secret());
        sessions.upsert_session_binding_payload(user_id, revlm::session_binding_hash(session.key), "web",
                                                "2099-01-01 00:00:00");
        const std::string path = "/api/token/" + std::to_string(token_id) + "/channel";
        const std::string get_response = revlm::handle_http_request(
            make_api_request("GET", path, user_id, session.value), false, "req-token-channel-get");
        if (expect_contains(get_response, "\"success\":true", "channel group GET should succeed") != 0 ||
            expect_contains(get_response, "\"channel_group_id\":" + std::to_string(enabled_group_id),
                            "GET payload should include bound channel_group_id") != 0 ||
            expect_contains(get_response, "\"name\":\"" + enabled_name + "\"",
                            "GET payload should include enabled group name") != 0 ||
            expect_contains(get_response,
                            "\"price_multiplier\":", "GET payload should include numeric price_multiplier") != 0 ||
            expect_contains(get_response, "\"allowed_channel_groups\"",
                            "GET payload should include allowed_channel_groups") != 0) {
            return 1;
        }

        const std::string put_body = "{\"channel_group_id\":" + std::to_string(alt_group_id) + "}";
        const std::string put_response = revlm::handle_http_request(
            make_api_request("PUT", path, user_id, session.value, put_body), false, "req-token-channel-put");
        if (expect_contains(put_response, "\"success\":true", "channel group PUT should succeed") != 0) {
            return 1;
        }

        const auto rebound = tokens.get_user_token_by_id(user_id, token_id);
        if (expect(rebound.has_value() && rebound->channel_group_id == alt_group_id,
                   "token should store the updated channel_group_id") != 0) {
            return 1;
        }

        const std::string reject_body = "{\"channel_group_id\":" + std::to_string(disabled_group_id) + "}";
        const std::string put_reject_response = revlm::handle_http_request(
            make_api_request("PUT", path, user_id, session.value, reject_body), false, "req-token-channel-put-reject");
        if (expect_contains(put_reject_response, "\"success\":false,\"message\":\"渠道组已禁用\"",
                            "disabled channel groups should be rejected") != 0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "token channel group contract test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
