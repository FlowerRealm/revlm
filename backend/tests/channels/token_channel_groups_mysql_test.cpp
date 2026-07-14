#include "channels/channel_groups.hpp"
#include "server/http_server.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"
#include "server/tokens.hpp"
#include "auth/session.hpp"
#include "auth/users.hpp"
#include "util/user_input.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping token channel-groups contract test\n";
        return 0;
    }

    try {
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);

        revlm::UserStore users(*db);
        revlm::SessionStore sessions(*db);
        revlm::ChannelGroupStore groups(*db);
        revlm::TokenStore &tokens = users.tokens();

        const auto now =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch());
        const std::string suffix = std::to_string(now.count());
        const std::string username = "tmpa003" + suffix;
        const std::string email = username + "@example.com";
        const std::string enabled_name = "tmpenabled" + suffix;
        const std::string legacy_name = "tmplegacy" + suffix;
        const std::string newcomer_name = "tmpnewdisabled" + suffix;

        revlm::User user_id_user = revlm::User(email, username, revlm::hash_password("password123"), "user");
        user_id_user.status = 1;
        const long long user_id = users.create_user(std::move(user_id_user));
        const long long enabled_id = groups.create_channel_group(enabled_name, "", 1.0, 1);
        const long long legacy_id = groups.create_channel_group(legacy_name, "", 1.0, 1);
        const long long newcomer_id = groups.create_channel_group(newcomer_name, "", 1.0, 0);
        (void)enabled_id;
        (void)newcomer_id;

        const long long token_id = tokens.create_user_token(user_id, odb::nullable<std::string>{"tmp contract token"},
                                                            "sk_tmp_contract_" + suffix);
        if (expect(tokens.replace_token_channel_groups(token_id, { enabled_name, legacy_name }),
                   "initial token channel-group bind should succeed") != 0) {
            return 1;
        }
        revlm::sql_exec(*db, "UPDATE channel_groups SET status=0 WHERE id=" + std::to_string(legacy_id));
        if (expect(true, "disabling a currently bound channel group should succeed") != 0) {
            return 1;
        }

        revlm::Config config;
        config.db_dsn = dsn;
        config.session_secret = "tmp-a003-contract-secret";
        const revlm::SessionCookie session =
            revlm::make_session_cookie(user_id, revlm::session_secret_for_config(config));
        sessions.upsert_session_binding_payload(user_id, revlm::session_binding_hash(session.key), "web",
                                                "2099-01-01 00:00:00");

        const revlm::BuildInfo build{ "test-version", "test-date" };
        const std::string path = "/api/token/" + std::to_string(token_id) + "/channel-groups";
        const std::string get_response = revlm::handle_http_request(
            make_api_request("GET", path, user_id, session.value), config, build, false, "req-token-groups-get");
        if (expect_contains(get_response, "\"success\":true", "channel-groups GET should succeed") != 0 ||
            expect_contains(get_response, "\"name\":\"" + legacy_name + "\"",
                            "GET payload should include disabled bound group name") != 0 ||
            expect_contains(get_response, "\"status\":0", "GET payload should include disabled bound group status") !=
                0 ||
            expect_contains(get_response,
                            "\"price_multiplier\":", "GET payload should include numeric price_multiplier") != 0 ||
            expect_contains(get_response, "\"channel_group_name\":\"" + legacy_name + "\"",
                            "GET payload should still include disabled binding order") != 0) {
            return 1;
        }

        const std::string same_body = "{\"channel_groups\":[\"" + enabled_name + "\",\"" + legacy_name + "\"]}";
        const std::string put_same_response =
            revlm::handle_http_request(make_api_request("PUT", path, user_id, session.value, same_body), config, build,
                                       false, "req-token-groups-put-same");
        if (expect_contains(put_same_response, "\"success\":true",
                            "saving the same disabled binding set should succeed") != 0) {
            return 1;
        }

        const std::vector<revlm::TokenChannelGroupBinding> rebound = tokens.list_token_channel_group_bindings(token_id);
        if (expect(rebound.size() == 2, "token should still have exactly two bindings after save") != 0 ||
            expect(rebound[0].channel_group_name == enabled_name && rebound[1].channel_group_name == legacy_name,
                   "saving should preserve the existing disabled binding order") != 0) {
            return 1;
        }

        const std::string reject_body =
            "{\"channel_groups\":[\"" + enabled_name + "\",\"" + legacy_name + "\",\"" + newcomer_name + "\"]}";
        const std::string put_reject_response =
            revlm::handle_http_request(make_api_request("PUT", path, user_id, session.value, reject_body), config,
                                       build, false, "req-token-groups-put-reject");
        if (expect_contains(put_reject_response,
                            "\"success\":false,\"message\":\"渠道组已禁用: " + newcomer_name + "\"",
                            "new disabled groups should still be rejected") != 0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "token channel-groups contract test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
