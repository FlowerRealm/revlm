// What the executor still owns after ADR 0006/0007: URL validation and joining,
// the SSRF guard, and dropping hop-by-hop headers. Authentication headers and
// protocol-shaped retries moved into the plugins, so there is nothing here that
// knows an OpenAI request from an Anthropic one -- the two channels below differ
// only in their base URL.
#include "proxy/upstream.hpp"

#include "channels/channels.hpp"
#include "config/config.hpp"
#include "store/database.hpp"
#include "store/mysql_test_env.hpp"
#include "store/schema.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
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

std::string header_value(const std::vector<revlm::UpstreamHeader> &headers, std::string_view name)
{
    for (const auto &header : headers) {
        std::string left = header.name;
        std::string right = std::string{ name };
        for (char &ch : left) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        for (char &ch : right) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        if (left == right) {
            return header.value;
        }
    }
    return {};
}

long long seed_channel(std::string_view type, std::string_view name, std::string_view base_url,
                       std::string_view api_key)
{
    revlm::Channel channel(0, std::string{ type }, std::string{ name }, true, 0, std::string{ base_url },
                           std::string{ api_key }, 1.0);
    if (!revlm::ChannelStore::instance().create_channel(channel)) {
        throw std::runtime_error("create_channel failed");
    }
    return channel.id;
}

} // namespace

int main()
{
    std::optional<revlm::test::MysqlTestEnv> env = revlm::test::prepare_mysql_test_env("upstream");
    if (!env.has_value()) {
        return 0;
    }

    try {
        auto db = revlm::make_database(env->dsn);
        revlm::ensure_schema(*db);
        revlm::Config config;
        config.db_dsn = env->dsn;
        revlm::test::install_test_runtime(config);
        revlm::sql_exec(*db, "DELETE FROM channels");
    } catch (const std::exception &ex) {
        std::cerr << "mysql setup failed: " << ex.what() << '\n';
        return 1;
    }

    const long long prefixed_id =
        seed_channel("openai_compatible", "prefixed", "https://api.example.test/v1", "sk-key");
    const long long bare_id = seed_channel("anthropic", "bare", "https://claude.example.test", "sk-other");
    const long long blocked_id = seed_channel("openai_compatible", "blocked", "http://127.0.0.1:18080", "sk-blocked");

    revlm::UpstreamExecutor executor;

    {
        revlm::UpstreamRequest request;
        request.method = "POST";
        request.path = "/v1/responses";
        request.body = R"({"model":"gpt-5"})";
        bool rejected = false;
        try {
            (void)executor.prepare(blocked_id, request);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        if (expect(rejected, "executor should reject blocked SSRF upstream targets") != 0) {
            return 1;
        }
    }

    {
        revlm::UpstreamRequest request{
            .method = "POST",
            .path = "/v1/responses",
            .query = "stream=true",
            .headers = { { "Authorization", "Bearer sk-plugin" },
                         { "Accept-Encoding", "identity" },
                         { "X-Test", "ok" },
                         { "Connection", "keep-alive" },
                         { "X-Forwarded-For", "10.0.0.1" } },
            .body = R"({"model":"gpt-5","stream":true})",
        };
        const auto prepared = executor.prepare(prefixed_id, request);
        if (expect(prepared.url == "https://api.example.test/v1/responses?stream=true",
                   "a /v1 base should collapse the downstream /v1 prefix") != 0 ||
            // The plugin's own authentication, not the channel key: the executor
            // adds no header of its own and rewrites none of the plugin's.
            expect(header_value(prepared.headers, "authorization") == "Bearer sk-plugin",
                   "executor should pass plugin authentication through untouched") != 0 ||
            expect(header_value(prepared.headers, "accept-encoding") == "identity",
                   "executor should preserve the encoding the plugin asked for") != 0 ||
            expect(header_value(prepared.headers, "x-test") == "ok", "executor should preserve safe headers") != 0 ||
            expect(header_value(prepared.headers, "connection").empty(), "executor should drop hop-by-hop headers") !=
                0 ||
            expect(header_value(prepared.headers, "x-forwarded-for").empty(),
                   "executor should drop forwarding headers") != 0 ||
            expect(prepared.body == R"({"model":"gpt-5","stream":true})",
                   "executor should not rewrite the plugin's body") != 0) {
            return 1;
        }
    }

    {
        revlm::UpstreamRequest request{
            .method = "POST",
            .path = "/v1/messages",
            .query = {},
            .headers = { { "x-api-key", "sk-plugin" } },
            .body = R"({"model":"claude","stream":false})",
        };
        const auto prepared = executor.prepare(bare_id, request);
        if (expect(prepared.url == "https://claude.example.test/v1/messages",
                   "a prefix-free base should keep the downstream path") != 0 ||
            expect(header_value(prepared.headers, "x-api-key") == "sk-plugin",
                   "executor should pass any auth scheme through unchanged") != 0) {
            return 1;
        }
    }

    {
        // One attempt, one response: a 4xx is handed back as-is. Deciding whether
        // it is worth rewriting and resending is protocol knowledge and lives in
        // the plugin's own attempt (ADR 0009).
        revlm::UpstreamRequest request{
            .method = "POST",
            .path = "/v1/responses",
            .query = {},
            .headers = {},
            .body = R"({"model":"gpt-5","max_output_tokens":16})",
        };
        int calls = 0;
        const auto result = executor.execute(
            prefixed_id, request, [&](const revlm::UpstreamPreparedRequest &prepared) -> revlm::UpstreamResponse {
                ++calls;
                if (expect(prepared.body.find("\"max_output_tokens\"") != std::string::npos,
                           "executor should send the body the plugin built") != 0) {
                    throw std::runtime_error("body mismatch");
                }
                return revlm::UpstreamResponse{
                    .status_code = 400,
                    .headers = {},
                    .body = R"({"error":{"message":"Unsupported parameter: max_output_tokens"}})",
                };
            });
        if (expect(calls == 1, "executor should call the transport exactly once") != 0 ||
            expect(result.response.status_code == 400, "executor should return the 4xx unchanged") != 0) {
            return 1;
        }
    }

    return 0;
}
