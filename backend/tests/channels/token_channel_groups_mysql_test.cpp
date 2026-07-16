#include "channels/channels.hpp"
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
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping token channel contract test\n";
        return 0;
    }

    try {
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);

        revlm::UserStore users(*db);
        revlm::SessionStore sessions(*db);
        revlm::ChannelStore channels(*db);
        revlm::TokenStore &tokens = users.tokens();

        const auto now =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch());
        const std::string suffix = std::to_string(now.count());
        const std::string username = "tmpa003" + suffix;
        const std::string email = username + "@example.com";

        revlm::User user_id_user = revlm::User(email, username, revlm::hash_password("password123"), "user");
        user_id_user.status = 1;
        const long long user_id = users.create_user(std::move(user_id_user));

        revlm::Channel enabled_ch;
        enabled_ch.type = 2;
        enabled_ch.name = "tmp-enabled-" + suffix;
        enabled_ch.status = true;
        enabled_ch.base_url = "https://api.openai.com/v1";
        enabled_ch.api_key = "sk-enabled";
        enabled_ch.price_multiplier = 1.25;
        if (!channels.create_channel(enabled_ch)) {
            std::cerr << "failed to create enabled channel\n";
            return 1;
        }

        revlm::Channel alt_ch;
        alt_ch.type = 2;
        alt_ch.name = "tmp-alt-" + suffix;
        alt_ch.status = true;
        alt_ch.base_url = "https://api.openai.com/v1";
        alt_ch.api_key = "sk-alt";
        if (!channels.create_channel(alt_ch)) {
            std::cerr << "failed to create alt channel\n";
            return 1;
        }

        revlm::Channel disabled_ch;
        disabled_ch.type = 2;
        disabled_ch.name = "tmp-disabled-" + suffix;
        disabled_ch.status = false;
        disabled_ch.base_url = "https://api.openai.com/v1";
        disabled_ch.api_key = "sk-disabled";
        if (!channels.create_channel(disabled_ch)) {
            std::cerr << "failed to create disabled channel\n";
            return 1;
        }

        const long long token_id = tokens.create_user_token(user_id, odb::nullable<std::string>{"tmp contract token"},
                                                            "sk_tmp_contract_" + suffix);
        if (expect(tokens.set_token_channel(user_id, token_id, enabled_ch.id),
                   "initial token channel bind should succeed") != 0) {
            return 1;
        }

        revlm::Config config;
        config.db_dsn = dsn;
        config.session_secret = "tmp-a003-contract-secret";
        const revlm::SessionCookie session =
            revlm::make_session_cookie(user_id, revlm::session_secret_for_config(config));
        sessions.upsert_session_binding_payload(user_id, revlm::session_binding_hash(session.key), "web",
                                                "2099-01-01 00:00:00");
        const std::string path = "/api/token/" + std::to_string(token_id) + "/channel";
        const std::string get_response = revlm::handle_http_request(
            make_api_request("GET", path, user_id, session.value), config, false, "req-token-channel-get");
        if (expect_contains(get_response, "\"success\":true", "channel GET should succeed") != 0 ||
            expect_contains(get_response, "\"channel_id\":" + std::to_string(enabled_ch.id),
                            "GET payload should include bound channel_id") != 0 ||
            expect_contains(get_response, "\"name\":\"" + enabled_ch.name + "\"",
                            "GET payload should include enabled channel name") != 0 ||
            expect_contains(get_response, "\"price_multiplier\":",
                            "GET payload should include numeric price_multiplier") != 0 ||
            expect_contains(get_response, "\"allowed_channels\"", "GET payload should include allowed_channels") != 0) {
            return 1;
        }

        const std::string put_body = "{\"channel_id\":" + std::to_string(alt_ch.id) + "}";
        const std::string put_response =
            revlm::handle_http_request(make_api_request("PUT", path, user_id, session.value, put_body), config, false,
                                       "req-token-channel-put");
        if (expect_contains(put_response, "\"success\":true", "channel PUT should succeed") != 0) {
            return 1;
        }

        const auto rebound = tokens.get_user_token_by_id(user_id, token_id);
        if (expect(rebound.has_value() && rebound->channel_id == alt_ch.id,
                   "token should store the updated channel_id") != 0) {
            return 1;
        }

        const std::string reject_body = "{\"channel_id\":" + std::to_string(disabled_ch.id) + "}";
        const std::string put_reject_response =
            revlm::handle_http_request(make_api_request("PUT", path, user_id, session.value, reject_body), config, false,
                                       "req-token-channel-put-reject");
        if (expect_contains(put_reject_response, "\"success\":false,\"message\":\"渠道已禁用\"",
                            "disabled channels should be rejected") != 0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "token channel contract test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
