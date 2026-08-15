// End-to-end safety net for the streaming/SSE behaviour that P3
// (docs/plugin-v3-rewrite-plan.md) is about to relocate out of core into
// plugins. Every assertion here is against observable HTTP bytes returned by
// revlm::handle_http_request(...) and rows committed to the `requests`
// table -- never against Gateway, ProxyUpstreamResponse, UpstreamSession,
// apply_upstream_gateway_stream, or handle_sse_event. That is deliberate:
// these tests must still make sense (and still compile) after P3 deletes
// class Gateway.
//
// Pins (see docs/plugin-v3-rewrite-plan.md P3 risk note):
//   1. non-streaming proxied request: status/body passthrough + usage rows.
//   2. streaming (SSE) request: exact byte sequence forwarded to the client,
//      including the terminating "data: [DONE]" frame.
//   3. first_token_latency_ms is recorded and reflects a delayed first chunk.
//   4. upstream SSE split mid-event across two TCP writes is reassembled
//      byte-exact for the client AND usage extraction survives the split.
//   6. no available channel -> error shape returned to the client.
//
// (Behaviour 5, client-disconnect-mid-stream, needs a real listening socket
// to sever mid-response and lives in streaming_disconnect_mysql_test.cpp.)
//
// Named *_mysql_test (matching http_server_mysql_chat_completions_test.cpp
// and friends) rather than using the prepare_mysql_test_env() docker
// fallback: /v1/chat/completions is registered by the OpenAI plugin via
// symbol interposition (revlm_register_http_routes), and on Darwin that
// relies on DYLD_INSERT_LIBRARIES rewriting an already-bound core symbol,
// which Mach-O two-level namespace binding does not allow (see
// backend/tests/plugins/preload_symbols_test.cpp). backend/tests/CMakeLists.txt
// only wires LD_PRELOAD for `if(NOT APPLE)`, so this route is only reachable
// in CI (Linux). Locally this test auto-skips exactly like its siblings.

#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "server/http_server.hpp"
#include "store/database.hpp"
#include "store/mysql_test_env.hpp"
#include "store/schema.hpp"
#include "plugins/host.hpp"

#include <httplib.h>
#include "users/tokens.hpp"
#include "users/users.hpp"
#include "util/json.hpp"
#include "util/user_input.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

int g_failures = 0;

void expect(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

// Reverses HTTP/1.1 "Transfer-Encoding: chunked" framing so tests can assert
// byte-exact equality against what the upstream actually sent, instead of a
// loose substring match. httplib always chunk-encodes bodies produced via
// set_chunked_content_provider(), which is what the streaming code path uses.
std::string dechunk_http_body(const std::string &raw_response)
{
    const auto head_end = raw_response.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        return {};
    }
    const std::string body = raw_response.substr(head_end + 4);
    std::string out;
    size_t pos = 0;
    while (pos < body.size()) {
        const size_t line_end = body.find("\r\n", pos);
        if (line_end == std::string::npos) {
            break;
        }
        std::string size_hex = body.substr(pos, line_end - pos);
        const size_t semi = size_hex.find(';');
        if (semi != std::string::npos) {
            size_hex = size_hex.substr(0, semi);
        }
        size_t chunk_size = 0;
        try {
            chunk_size = static_cast<size_t>(std::stoul(size_hex, nullptr, 16));
        } catch (const std::exception &) {
            break;
        }
        pos = line_end + 2;
        if (chunk_size == 0) {
            break;
        }
        if (pos + chunk_size > body.size()) {
            break;
        }
        out.append(body, pos, chunk_size);
        pos += chunk_size + 2; // skip the chunk's trailing CRLF
    }
    return out;
}

struct StreamStep {
    int delay_ms = 0;
    std::string bytes;
};

// Fake upstream that speaks raw HTTP over a real TCP loopback socket, same
// shape as the MockUpstreamServer used throughout backend/tests/. The only
// addition is that the response can be split into multiple send() calls with
// delays between them, so tests can force a chunk boundary (or SSE-event
// boundary) to land in the middle of a TCP write.
struct MockUpstreamServer {
    int port = 0;
    std::string captured_request;
    std::thread thread;
    std::atomic_bool ready{ false };

    void start(std::vector<StreamStep> steps)
    {
        int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(listener, 1) != 0) {
            std::cerr << "mock upstream listen failed: " << std::strerror(errno) << '\n';
            std::exit(1);
        }
        socklen_t len = sizeof(addr);
        (void)::getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &len);
        port = ntohs(addr.sin_port);
        thread = std::thread([this, listener, steps = std::move(steps)]() mutable {
            ready.store(true);
            pollfd pfd{};
            pfd.fd = listener;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 5000) <= 0) {
                ::close(listener);
                return;
            }
            int client = ::accept(listener, nullptr, nullptr);
            ::close(listener);
            if (client < 0) {
                return;
            }
            char buffer[4096];
            while (true) {
                const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
                if (n <= 0) {
                    break;
                }
                captured_request.append(buffer, static_cast<size_t>(n));
                const auto head_end = captured_request.find("\r\n\r\n");
                if (head_end == std::string::npos) {
                    continue;
                }
                const std::string marker = "Content-Length:";
                const auto pos = captured_request.find(marker);
                if (pos == std::string::npos) {
                    break;
                }
                const auto line_end = captured_request.find("\r\n", pos);
                const std::string value = captured_request.substr(pos + marker.size(), line_end - pos - marker.size());
                const size_t body_len = static_cast<size_t>(std::stoll(value));
                if (captured_request.size() >= head_end + 4 + body_len) {
                    break;
                }
            }
            for (const StreamStep &step : steps) {
                if (step.delay_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(step.delay_ms));
                }
                (void)::send(client, step.bytes.data(), step.bytes.size(), MSG_NOSIGNAL);
            }
            ::shutdown(client, SHUT_WR);
            ::close(client);
        });
        for (int i = 0; i < 200 && !ready.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void start(std::string response)
    {
        std::vector<StreamStep> steps;
        steps.push_back(StreamStep{ 0, std::move(response) });
        start(std::move(steps));
    }

    void join()
    {
        if (thread.joinable()) {
            thread.join();
        }
    }

    ~MockUpstreamServer()
    {
        join();
    }
};

std::string api_request(std::string_view target, std::string_view token, std::string_view body,
                        std::string_view request_id)
{
    std::string req = "POST " + std::string(target) + " HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " +
                      std::string(token) + "\r\nX-Request-Id: " + std::string(request_id) +
                      "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                      "\r\n\r\n" + std::string(body);
    return revlm::handle_http_request(req, false);
}

} // namespace

int main()
{
    // prepare_mysql_test_env rather than a bare REVLM_TEST_MYSQL_DSN check: the
    // bare check made this test skip silently wherever that variable is unset,
    // so it could stay green for months without ever running. This starts its
    // own container when the variable is missing.
    const auto mysql_env = revlm::test::prepare_mysql_test_env("streaming e2e");
    if (!mysql_env.has_value()) {
        return 0;
    }
    const std::string dsn = mysql_env->dsn;

    try {
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);
        revlm::Config config;
        config.db_dsn = dsn;
        revlm::test::install_test_runtime(config);

        // Load the real packages: dlopen + each plugin's own registration is the
        // only way a /v1 route exists. handle_http_request() rebuilds its server
        // per call and deliberately never loads plugins, so this once-per-process
        // call is what every proxy request below travels through.
        ::httplib::Server plugin_host;
        revlm::plugin::load_plugins(plugin_host);

        revlm::sql_exec(*db, "DELETE FROM requests");
        revlm::sql_exec(*db, "DELETE FROM channel_group_members");
        revlm::sql_exec(*db, "DELETE FROM channel_groups");
        revlm::sql_exec(*db, "DELETE FROM channels");
        revlm::sql_exec(*db, "DELETE FROM user_tokens");
        revlm::sql_exec(*db, "DELETE FROM sessions");
        revlm::sql_exec(*db, "DELETE FROM users");

        revlm::UserStore &user_store = revlm::UserStore::instance();
        revlm::User user("streaming-e2e@example.com", "streaming-e2e", revlm::hash_password("password"), "user");
        user.status = 1;
        const long long user_id = user_store.create_user(std::move(user));
        revlm::User funded = user_store.get_user_by_id(user_id);
        funded.balance_usd = 100.0;
        expect(user_store.update_user(funded), "failed to fund test user");

        revlm::TokenStore &token_store = user_store.tokens();
        const std::string raw_token = "sk_tmp_streaming_e2e";
        const long long token_id = token_store.create_user_token(user_id, odb::nullable<std::string>{}, raw_token);

        revlm::ChannelStore &channel_store = revlm::ChannelStore::instance();
        revlm::ChannelGroupStore &group_store = revlm::ChannelGroupStore::instance();

        auto bind_channel = [&](revlm::Channel &channel) {
            expect(channel_store.create_channel(channel), "create channel failed");
            const int group_id =
                group_store.create_channel_group("g-" + channel.name, "", 1.0, true, "openai_compatible");
            expect(group_store.add_channel_group_member(group_id, channel), "add channel group member failed");
            expect(token_store.set_token_channel_group(user_id, token_id, group_id), "bind token channel group failed");
        };

        // ---- Behaviour 1: non-streaming proxied request ------------------
        {
            std::cerr << "[streaming-e2e] non-stream\n";
            MockUpstreamServer upstream;
            upstream.start("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                           "{\"id\":\"chatcmpl-nonstream\",\"object\":\"chat.completion\",\"model\":\"gpt-5.5\","
                           "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"hi\"}}],"
                           "\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":5,\"total_tokens\":17,"
                           "\"prompt_tokens_details\":{\"cached_tokens\":0}}}");
            revlm::Channel channel(0, "openai_compatible", "nonstream-ch", true, 10,
                                   "http://127.0.0.1:" + std::to_string(upstream.port), "upstream-secret-1");
            bind_channel(channel);
            const long long channel_id = channel.id;

            const std::string body = "{\"model\":\"gpt-5.5\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
            const std::string response = api_request("/v1/chat/completions", raw_token, body, "e2e-nonstream-1");
            upstream.join();

            expect(contains(response, "HTTP/1.1 200 OK"), "non-stream request should return 200");
            expect(contains(response, "\"chatcmpl-nonstream\""), "non-stream response body should pass through");
            expect(contains(upstream.captured_request, "Authorization: Bearer upstream-secret-1"),
                   "upstream should receive channel api key");

            // Token counts and is_stream are protocol-shaped detail that now
            // lives inside usage_details, which the core never parses (ADR
            // 0004); asserting into its field paths would re-couple the core
            // to a protocol. Assert only what the core owns.
            const auto rows =
                revlm::sql_query_rows(*db, "SELECT status_code,channel_id,usd,usage_details "
                                           "FROM requests WHERE request_id='e2e-nonstream-1' ORDER BY id DESC LIMIT 2");
            expect(rows.size() == 1, "non-stream request should commit exactly one usage row");
            if (rows.size() == 1) {
                expect(rows[0][0].value_or("") == "200", "non-stream row should record status 200");
                expect(rows[0][1].value_or("") == std::to_string(channel_id),
                       "non-stream row should record the channel it used");
                // The plugin prices its own protocol now (ADR 0004), so a
                // successful request costs something. How much is the plugin's
                // business; that it was charged at all is the core's.
                expect(std::stod(rows[0][2].value_or("0")) > 0.0,
                       "non-stream row should carry the charge the plugin priced");
                const auto usage_details = rows[0][3].value_or("{}");
                expect(usage_details != "{}" && revlm::json::parse(usage_details).has_value(),
                       "non-stream row should persist the usage payload the plugin extracted");
            }
        }

        // ---- Behaviour 2: streaming SSE forwarded byte-exact, including
        //      the terminating "data: [DONE]" frame. -----------------------
        {
            std::cerr << "[streaming-e2e] stream exact bytes\n";
            revlm::sql_exec(*db, "DELETE FROM requests");
            const std::string sse_payload =
                "data: {\"id\":\"chatcmpl-stream\",\"object\":\"chat.completion.chunk\",\"model\":\"gpt-5.5\","
                "\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
                "data: {\"id\":\"chatcmpl-stream\",\"object\":\"chat.completion.chunk\",\"model\":\"gpt-5.5\","
                "\"usage\":{\"prompt_tokens\":8,\"completion_tokens\":4,\"total_tokens\":12,"
                "\"prompt_tokens_details\":{\"cached_tokens\":0}},\"choices\":[]}\n\n"
                "data: [DONE]\n\n";
            MockUpstreamServer upstream;
            upstream.start("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" +
                           sse_payload);
            revlm::Channel channel(0, "openai_compatible", "stream-ch", true, 10,
                                   "http://127.0.0.1:" + std::to_string(upstream.port), "upstream-secret-2");
            bind_channel(channel);
            const long long channel_id = channel.id;

            const std::string body =
                "{\"model\":\"gpt-5.5\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
            const std::string response = api_request("/v1/chat/completions", raw_token, body, "e2e-stream-1");
            upstream.join();

            expect(contains(response, "HTTP/1.1 200 OK"), "stream request should return 200");
            expect(contains(response, "text/event-stream"), "stream response should preserve SSE content type");
            const std::string dechunked = dechunk_http_body(response);
            expect(dechunked == sse_payload,
                   "stream response body should reach the client byte-exact, DONE marker included");

            const auto rows = revlm::sql_query_rows(*db, "SELECT channel_id,usage_details "
                                                         "FROM requests WHERE request_id='e2e-stream-1' "
                                                         "ORDER BY id DESC LIMIT 2");
            expect(rows.size() == 1, "stream request should commit exactly one usage row");
            if (rows.size() == 1) {
                expect(rows[0][0].value_or("") == std::to_string(channel_id), "stream row should record channel id");
                const auto usage_details = rows[0][1].value_or("{}");
                expect(usage_details != "{}" && revlm::json::parse(usage_details).has_value(),
                       "stream row should persist the usage payload the plugin extracted");
            }
        }

        // ---- Behaviour 3: first_token_latency_ms reflects a delayed first
        //      chunk. ----------------------------------------------------
        {
            std::cerr << "[streaming-e2e] first token latency\n";
            revlm::sql_exec(*db, "DELETE FROM requests");
            constexpr int kInjectedDelayMs = 300;
            std::vector<StreamStep> steps;
            steps.push_back(StreamStep{ 0, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                           "Connection: close\r\n\r\n" });
            steps.push_back(StreamStep{ kInjectedDelayMs,
                                        "data: {\"id\":\"chatcmpl-latency\",\"object\":\"chat.completion.chunk\","
                                        "\"model\":\"gpt-5.5\",\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n" });
            steps.push_back(StreamStep{
                0, "data: {\"id\":\"chatcmpl-latency\",\"object\":\"chat.completion.chunk\",\"model\":\"gpt-5.5\","
                   "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":2,\"total_tokens\":5,"
                   "\"prompt_tokens_details\":{\"cached_tokens\":0}},\"choices\":[]}\n\n"
                   "data: [DONE]\n\n" });
            MockUpstreamServer upstream;
            upstream.start(std::move(steps));
            revlm::Channel channel(0, "openai_compatible", "latency-ch", true, 10,
                                   "http://127.0.0.1:" + std::to_string(upstream.port), "upstream-secret-3");
            bind_channel(channel);

            const std::string body =
                "{\"model\":\"gpt-5.5\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
            const std::string response = api_request("/v1/chat/completions", raw_token, body, "e2e-latency-1");
            upstream.join();
            expect(contains(response, "HTTP/1.1 200 OK"), "delayed-first-chunk stream should still succeed");

            const auto rows = revlm::sql_query_rows(*db, "SELECT first_token_latency_ms FROM requests "
                                                         "WHERE request_id='e2e-latency-1' ORDER BY id DESC LIMIT 1");
            expect(!rows.empty(), "delayed-first-chunk request should commit a usage row");
            if (!rows.empty()) {
                const int recorded = std::atoi(rows[0][0].value_or("0").c_str());
                // Allow generous scheduling slack; the point is that it tracks
                // the injected delay rather than being 0 or the full request
                // latency.
                expect(recorded >= kInjectedDelayMs - 150,
                       "first_token_latency_ms should reflect the delayed first chunk");
                expect(recorded > 0, "first_token_latency_ms should be > 0 for a delayed stream");
            }
        }

        // ---- Behaviour 4: SSE event split mid-frame across two TCP writes
        //      still reassembles byte-exact for the client and for usage
        //      extraction. ----------------------------------------------
        {
            std::cerr << "[streaming-e2e] split-frame reassembly\n";
            revlm::sql_exec(*db, "DELETE FROM requests");
            // Split this SSE event in the middle of the JSON object, and even
            // mid-line inside the "data:" field, across two separate send()
            // calls with a real delay between them so they cannot coalesce
            // into a single TCP segment.
            const std::string full_line =
                "data: {\"id\":\"chatcmpl-split\",\"object\":\"chat.completion.chunk\",\"model\":\"gpt-5.5\","
                "\"usage\":{\"prompt_tokens\":6,\"completion_tokens\":9,\"total_tokens\":15,"
                "\"prompt_tokens_details\":{\"cached_tokens\":0}},\"choices\":[]}\n\n";
            const size_t split_at = full_line.find("\"completion_tokens\"");
            expect(split_at != std::string::npos, "test setup: split point must exist in the payload");
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                     "Connection: close\r\n\r\n";
            std::vector<StreamStep> steps;
            steps.push_back(StreamStep{ 0, head + full_line.substr(0, split_at) });
            steps.push_back(StreamStep{ 50, full_line.substr(split_at) + "data: [DONE]\n\n" });
            MockUpstreamServer upstream;
            upstream.start(std::move(steps));
            revlm::Channel channel(0, "openai_compatible", "split-ch", true, 10,
                                   "http://127.0.0.1:" + std::to_string(upstream.port), "upstream-secret-4");
            bind_channel(channel);

            const std::string body =
                "{\"model\":\"gpt-5.5\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
            const std::string response = api_request("/v1/chat/completions", raw_token, body, "e2e-split-1");
            upstream.join();

            expect(contains(response, "HTTP/1.1 200 OK"), "split-frame stream should still succeed");
            const std::string dechunked = dechunk_http_body(response);
            expect(dechunked == full_line + "data: [DONE]\n\n",
                   "client should receive the split SSE event reassembled byte-exact");

            // The byte-exact client-side assertion above already proves the TCP
            // split reassembled correctly; what usage the plugin extracted from
            // it is protocol detail the core doesn't parse (ADR 0004). Just
            // confirm the reassembled payload made it into a committed row.
            const auto rows = revlm::sql_query_rows(*db, "SELECT usage_details FROM requests "
                                                         "WHERE request_id='e2e-split-1' ORDER BY id DESC LIMIT 2");
            expect(rows.size() == 1, "split-frame request should still commit exactly one usage row");
            if (rows.size() == 1) {
                const auto usage_details = rows[0][0].value_or("{}");
                expect(usage_details != "{}" && revlm::json::parse(usage_details).has_value(),
                       "split-frame usage extraction should survive the TCP split");
            }
        }

        // ---- Behaviour 6: no available channel -> error shape -----------
        {
            std::cerr << "[streaming-e2e] no available channel\n";
            revlm::Channel disabled(0, "openai_compatible", "disabled-ch", /*status=*/false, 10, "http://127.0.0.1:1",
                                    "irrelevant-key");
            expect(channel_store.create_channel(disabled), "create disabled channel failed");
            const int group_id = group_store.create_channel_group("g-disabled", "", 1.0, true, "openai_compatible");
            expect(group_store.add_channel_group_member(group_id, disabled),
                   "add disabled channel group member failed");
            expect(token_store.set_token_channel_group(user_id, token_id, group_id),
                   "bind disabled channel group failed");

            const std::string body = "{\"model\":\"gpt-5.5\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
            const std::string response = api_request("/v1/chat/completions", raw_token, body, "e2e-no-channel-1");

            // 503, not 400: the request is well-formed, there is simply no
            // upstream left to serve it (protocol_dispatch.cpp "no available
            // channel").
            expect(contains(response, "HTTP/1.1 503"), "no-available-channel should return 503");
            expect(contains(response, "\"no available channel\""),
                   "no-available-channel error body should carry the current error message");
        }

    } catch (const std::exception &err) {
        std::cerr << "streaming e2e test failed: " << err.what() << '\n';
        return 1;
    }

    return g_failures == 0 ? 0 : 1;
}
