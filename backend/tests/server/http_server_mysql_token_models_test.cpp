#include "auth/users.hpp"
#include "util/user_input.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "server/http_server.hpp"
#include "server/tokens.hpp"
#include "store/database.hpp"

#include <cstdlib>
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

std::string api_request(std::string_view method, std::string_view target, std::string_view token_header_name,
                        std::string_view token, std::string_view request_id)
{
    std::string req = std::string(method) + " " + std::string(target) + " HTTP/1.1\r\nHost: test\r\n" +
                      std::string(token_header_name) + ": " + std::string(token) + "\r\n\r\n";
    revlm::Config config;
    config.db_dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    return revlm::handle_http_request(req, config, revlm::BuildInfo{ "test-version", "test-date" }, false, request_id);
}

} // namespace

int main()
{
    const char *dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping token model MySQL test\n";
        return 0;
    }

    try {
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);

        revlm::sql_exec(*db, "DELETE FROM requests");
        revlm::sql_exec(*db, "DELETE FROM channel_group_members");
        revlm::sql_exec(*db, "DELETE FROM token_channel_groups");
        revlm::sql_exec(*db, "DELETE FROM channel_groups");
        revlm::sql_exec(*db, "DELETE FROM channels");
        revlm::sql_exec(*db, "DELETE FROM user_tokens");
        revlm::sql_exec(*db, "DELETE FROM session_bindings");
        revlm::sql_exec(*db, "DELETE FROM users");

        revlm::UserStore user_store(*db);
        revlm::User user_id_user =
            revlm::User("models@example.com", "models", revlm::hash_password("password"), "user");
        user_id_user.status = 1;
        const long long user_id = user_store.create_user(std::move(user_id_user));

        revlm::TokenStore &token_store = user_store.tokens();
        const std::string raw_token = "sk_tmp_g001_models";
        const long long token_id = token_store.create_user_token(user_id, odb::nullable<std::string>{}, raw_token);

        revlm::ChannelGroupStore group_store(*db);
        const long long openai_group_id = group_store.create_channel_group("tmp_g001_openai", "", 1.0);
        const long long anthropic_group_id = group_store.create_channel_group("tmp_g001_anthropic", "", 1.0);
        if (!token_store.replace_token_channel_groups(token_id, { "tmp_g001_openai" })) {
            std::cerr << "failed to bind token groups\n";
            return 1;
        }

        revlm::ChannelStore channel_store(*db);
        revlm::Channel openai_ch;
        openai_ch.type = 2;
        openai_ch.name = "tmp-g001-openai";
        openai_ch.status = 1;
        openai_ch.base_url = "https://api.openai.com/v1";
        if (!channel_store.create_channel(openai_ch)) {
            std::cerr << "failed to create openai channel\n";
            return 1;
        }
        if (!group_store.add_channel_group_member(openai_group_id, openai_ch)) {
            std::cerr << "failed to bind openai channel group member\n";
            return 1;
        }

        revlm::Channel anthropic_ch;
        anthropic_ch.type = 4;
        anthropic_ch.name = "tmp-g001-anthropic";
        anthropic_ch.status = 1;
        anthropic_ch.base_url = "https://api.anthropic.com";
        if (!channel_store.create_channel(anthropic_ch)) {
            std::cerr << "failed to create anthropic channel\n";
            return 1;
        }
        if (!group_store.add_channel_group_member(anthropic_group_id, anthropic_ch)) {
            std::cerr << "failed to bind anthropic channel group member\n";
            return 1;
        }

        const std::string list_bearer =
            api_request("GET", "/v1/models", "Authorization", "Bearer " + raw_token, "req-models-bearer");
        if (expect(contains(list_bearer, "HTTP/1.1 200 OK"), "bearer models list should succeed") != 0 ||
            expect(contains(list_bearer, "\"id\":\"gpt-5.5\""), "reachable openai model should be listed") != 0 ||
            expect(!contains(list_bearer, "\"id\":\"claude-opus-4-8\""),
                   "unreachable anthropic model should not be listed") != 0) {
            std::cerr << list_bearer << '\n';
            return 1;
        }

        const std::string list_api_key =
            api_request("GET", "/v1beta/openai/models", "x-api-key", raw_token, "req-models-x-api-key");
        if (expect(contains(list_api_key, "HTTP/1.1 200 OK"), "x-api-key models list should succeed") != 0 ||
            expect(contains(list_api_key, "\"id\":\"gpt-5.3-codex\""), "v1beta list should share model catalog") != 0) {
            std::cerr << list_api_key << '\n';
            return 1;
        }

        const std::string retrieve_model =
            api_request("GET", "/v1/models/gpt-5.3-codex", "x-api-key", raw_token, "req-model-retrieve");
        if (expect(contains(retrieve_model, "HTTP/1.1 200 OK"), "model retrieve should succeed") != 0 ||
            expect(contains(retrieve_model, "\"id\":\"gpt-5.3-codex\""), "model retrieve should return requested id") !=
                0) {
            std::cerr << retrieve_model << '\n';
            return 1;
        }

        const std::string unreachable_model =
            api_request("GET", "/v1/models/claude-opus-4-8", "x-api-key", raw_token, "req-model-unreachable");
        if (expect(contains(unreachable_model, "HTTP/1.1 200 OK"),
                   "catalog model retrieve should succeed even when token cannot route") != 0 ||
            expect(contains(unreachable_model, "\"id\":\"claude-opus-4-8\""),
                   "catalog model retrieve should return requested id") != 0) {
            std::cerr << unreachable_model << '\n';
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "token model MySQL test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
