// Safety net for behaviour 5 from docs/plugin-v3-rewrite-plan.md's P3 risk
// note: "client disconnects mid-stream -> the core must stop reading
// upstream and must still commit the request record."
//
// This needs a real listening socket (not the in-memory BufferStream used by
// backend/tests/server/http_server_mysql_*_test.cpp) because we have to
// sever the TCP connection from the client side while a response is
// mid-flight, which an in-memory buffer can't represent. The pattern below
// -- revlm::HttpServer on a real loopback port, a hand-rolled upstream TCP
// server -- is copied from
// backend/tests/server/http_server_mysql_chat_completions_test.cpp.
//
// Assertions are entirely on: what the mock upstream observes happening on
// its own socket (does revlm keep writing to it / when does revlm close it),
// and what lands in the `requests` table. Nothing here names Gateway or any
// of its internal helper types.
//
// Named *_mysql_test: /v1/chat/completions is registered by the OpenAI
// plugin via symbol interposition, which only works on Linux (see the
// comment at the top of streaming_e2e_mysql_test.cpp and
// backend/tests/plugins/preload_symbols_test.cpp). Locally this auto-skips
// like its siblings; it runs for real in CI.

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

// The commit travels with the stream now (ADR 0009: the response head goes out
// before the body exists, so the record lands when the pump ends), which is
// after the mock upstream has finished its own writes. Poll rather than read
// once -- the alternative is a sleep long enough to be slow and short enough to
// be flaky.
std::vector<revlm::SqlResultRow> wait_for_rows(odb::database &db, const std::string &sql, std::size_t expected,
                                               int timeout_ms = 3000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::vector<revlm::SqlResultRow> rows;
    for (;;) {
        rows = revlm::sql_query_rows(db, sql);
        if (rows.size() >= expected || std::chrono::steady_clock::now() >= deadline) {
            return rows;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

constexpr int kServerPort = 18097;
// Must stay comfortably above gateway.cpp's kDisconnectDrainTimeoutMs
// (1500ms, the timeout applied to upstream polls once a client disconnect
// has been detected) so that "gave up quickly" and "gave up because it hit
// the full idle timeout" are distinguishable.
constexpr int kUpstreamTimeoutSeconds = 6;

int g_failures = 0;

void expect(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

std::string padded_sse_event(char fill, size_t size)
{
    std::string out = "data: ";
    out.append(size, fill);
    out += "\n\n";
    return out;
}

// Real, hand-rolled upstream: lets us send a response over several separate
// TCP writes with real delays between them (needed to force a chunk
// boundary after the client has already disconnected), and to observe
// whether/when revlm closes its side of the upstream connection.
struct DisconnectMockUpstream {
    int port = 0;
    std::string captured_request;
    std::thread thread;
    std::atomic_bool ready{ false };
    std::atomic_bool observed_upstream_eof{ false };
    std::atomic<long long> eof_observed_after_ms{ -1 };

    // steps[0] is sent immediately after the request is fully read. Each
    // subsequent step sleeps `delay_ms` first. After the last step, we poll
    // for the upstream socket to be closed by revlm (or a HUP/EOF) and
    // record how long that took, measured from right after the last step
    // was sent.
    void start(std::vector<std::pair<int, std::string>> steps)
    {
        int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(listener, 1) != 0) {
            std::cerr << "disconnect mock upstream listen failed: " << std::strerror(errno) << '\n';
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
                    ::close(client);
                    return;
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

            for (const auto &[delay_ms, bytes] : steps) {
                if (delay_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                }
                (void)::send(client, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            }

            const auto wait_start = std::chrono::steady_clock::now();
            pollfd watch{};
            watch.fd = client;
            watch.events = POLLIN;
            const int rc = ::poll(&watch, 1, (kUpstreamTimeoutSeconds + 2) * 1000);
            if (rc > 0) {
                char probe[16];
                const ssize_t n = ::recv(client, probe, sizeof(probe), 0);
                if (n == 0) {
                    observed_upstream_eof.store(true);
                    eof_observed_after_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    std::chrono::steady_clock::now() - wait_start)
                                                    .count());
                }
            }
            ::close(client);
        });
        for (int i = 0; i < 200 && !ready.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void join()
    {
        if (thread.joinable()) {
            thread.join();
        }
    }

    ~DisconnectMockUpstream()
    {
        join();
    }
};

struct HttpHarness {
    revlm::HttpServer server;
    std::atomic_bool running{ true };
    std::thread thread;

    void start()
    {
        thread = std::thread([&] { server.run(running); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    void stop()
    {
        running.store(false);
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kServerPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            // just needed to unblock accept()
        }
        ::close(fd);
        if (thread.joinable()) {
            thread.join();
        }
    }
};

// Connects, sends a full streaming request, reads until at least
// `min_bytes` of response have arrived (proving the client genuinely
// received a live chunk of the stream), then hard-closes the socket without
// reading the rest -- simulating a client that walks away mid-stream.
void send_request_then_disconnect(std::string_view request, size_t min_bytes)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kServerPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        std::cerr << "connect failed: " << std::strerror(errno) << '\n';
        std::exit(1);
    }
    (void)::send(fd, request.data(), request.size(), MSG_NOSIGNAL);

    std::string received;
    char buffer[4096];
    while (received.size() < min_bytes) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        received.append(buffer, static_cast<size_t>(n));
    }
    ::close(fd);
}

} // namespace

int main()
{
    // prepare_mysql_test_env rather than a bare REVLM_TEST_MYSQL_DSN check: the
    // bare check made this test skip silently wherever that variable is unset,
    // so it could stay green for months without ever running. This starts its
    // own container when the variable is missing.
    const auto mysql_env = revlm::test::prepare_mysql_test_env("streaming disconnect");
    if (!mysql_env.has_value()) {
        return 0;
    }
    const std::string dsn = mysql_env->dsn;

    try {
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);
        revlm::Config config;
        config.db_dsn = dsn;
        config.addr = "127.0.0.1:" + std::to_string(kServerPort);
        config.proxy_upstream_timeout_seconds = kUpstreamTimeoutSeconds;
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
        revlm::User user("streaming-disconnect@example.com", "streaming-disconnect", revlm::hash_password("password"),
                         "user");
        user.status = 1;
        const long long user_id = user_store.create_user(std::move(user));
        revlm::User funded = user_store.get_user_by_id(user_id);
        funded.balance_usd = 100.0;
        expect(user_store.update_user(funded), "failed to fund test user");

        revlm::TokenStore &token_store = user_store.tokens();
        const std::string raw_token = "sk_tmp_streaming_disconnect";
        const long long token_id = token_store.create_user_token(user_id, odb::nullable<std::string>{}, raw_token);

        revlm::ChannelStore &channel_store = revlm::ChannelStore::instance();
        revlm::ChannelGroupStore &group_store = revlm::ChannelGroupStore::instance();

        // Channel/group/binding created once; base_url gets pointed at a
        // fresh mock upstream per scenario below.
        revlm::Channel channel(0, "openai_compatible", "disconnect-ch", true, 10, "http://127.0.0.1:1",
                               "upstream-secret");
        expect(channel_store.create_channel(channel), "create channel failed");
        const int group_id = group_store.create_channel_group("g-disconnect", "", 1.0, true, "openai_compatible");
        expect(group_store.add_channel_group_member(group_id, channel), "add channel group member failed");
        expect(token_store.set_token_channel_group(user_id, token_id, group_id), "bind channel group failed");

        const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
        const std::string filler_a = padded_sse_event('a', 1200);
        const std::string filler_b = padded_sse_event('b', 1200);
        const std::string filler_c = padded_sse_event('c', 1200);

        HttpHarness harness;
        harness.start();

        // ---- Scenario A: client disconnects, upstream still manages to
        //      finish (usage + [DONE]) inside the short post-disconnect
        //      drain window -> request record must still be committed. -----
        {
            std::cerr << "[streaming-disconnect] disconnect-then-finish\n";
            revlm::sql_exec(*db, "DELETE FROM requests");
            const std::string final_event =
                "data: {\"id\":\"chatcmpl-disc-a\",\"object\":\"chat.completion.chunk\",\"model\":\"gpt-5.5\","
                "\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":1,\"total_tokens\":3,"
                "\"prompt_tokens_details\":{\"cached_tokens\":0}},\"choices\":[]}\n\n"
                "data: [DONE]\n\n";
            DisconnectMockUpstream upstream;
            upstream.start({
                { 0, head + filler_a }, // client reads this, then disconnects
                { 150, filler_b }, // first write attempt after disconnect
                { 80, filler_c }, // extra attempts to guarantee the write
                // failure is observed by the TCP stack
                { 80, filler_c },
                { 80, final_event }, // usage + DONE, arrives inside the
                // 1.5s post-disconnect drain window
            });
            channel.base_url = "http://127.0.0.1:" + std::to_string(upstream.port);
            expect(channel_store.update_channel(channel), "failed to point channel at scenario A upstream");

            const std::string body =
                "{\"model\":\"gpt-5.5\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
            const std::string request = "POST /v1/chat/completions HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " +
                                        raw_token +
                                        "\r\nX-Request-Id: disc-a-1\r\nContent-Type: application/json\r\n"
                                        "Content-Length: " +
                                        std::to_string(body.size()) + "\r\n\r\n" + body;

            const auto scenario_start = std::chrono::steady_clock::now();
            send_request_then_disconnect(request, /*min_bytes=*/200);
            upstream.join();
            const auto scenario_elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - scenario_start)
                    .count();

            expect(upstream.observed_upstream_eof.load(),
                   "revlm should close its upstream connection once the drained stream completes");
            std::cerr << "  scenario A: total wall time " << scenario_elapsed << "ms\n";

            // Token counts and is_stream are protocol-shaped detail that lives
            // inside usage_details now, which the core never parses (ADR 0004);
            // asserting into its field paths would re-couple the core to a
            // protocol. Assert only what the core itself owns: the row exists
            // exactly once, carries the right channel/model, and usage_details
            // was actually filled in (not left at the unbilled default).
            const auto rows = wait_for_rows(*db,
                                            "SELECT status_code,channel_id,model,usage_details FROM requests "
                                            "WHERE request_id='disc-a-1' ORDER BY id DESC LIMIT 2",
                                            1);
            expect(rows.size() == 1,
                   "core must commit exactly one request record when upstream finishes after client disconnect");
            if (rows.size() == 1) {
                expect(rows[0][0].value_or("") == "200", "disconnected-but-finished stream should record status 200");
                expect(rows[0][1].value_or("") == std::to_string(channel.id),
                       "disconnected-but-finished stream should record the channel it used");
                expect(rows[0][2].value_or("") == "gpt-5.5", "disconnected-but-finished stream should record model");
                const auto usage_details = rows[0][3].value_or("{}");
                expect(usage_details != "{}" && revlm::json::parse(usage_details).has_value(),
                       "disconnected-but-finished stream should persist the usage payload it drained");
            }
        }

        // ---- Scenario B: client disconnects, upstream then goes silent ->
        //      core must give up within the short post-disconnect drain
        //      window (bounded, not the full idle timeout), and must NOT
        //      fabricate a usage row it never received. ---------------------
        {
            std::cerr << "[streaming-disconnect] disconnect-then-silence\n";
            revlm::sql_exec(*db, "DELETE FROM requests");
            DisconnectMockUpstream upstream;
            upstream.start({
                { 0, head + filler_a }, // client reads this, then disconnects
                { 150, filler_b }, // first write attempt after disconnect
                { 80, filler_c }, // extra attempts to guarantee the write
                // failure is observed by the TCP stack
                { 80, filler_c },
                // then: silence. No [DONE], no usage, ever.
            });
            channel.base_url = "http://127.0.0.1:" + std::to_string(upstream.port);
            expect(channel_store.update_channel(channel), "failed to point channel at scenario B upstream");

            const std::string body =
                "{\"model\":\"gpt-5.5\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
            const std::string request = "POST /v1/chat/completions HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " +
                                        raw_token +
                                        "\r\nX-Request-Id: disc-b-1\r\nContent-Type: application/json\r\n"
                                        "Content-Length: " +
                                        std::to_string(body.size()) + "\r\n\r\n" + body;

            send_request_then_disconnect(request, /*min_bytes=*/200);
            upstream.join();

            expect(upstream.observed_upstream_eof.load(),
                   "revlm should give up on a silent upstream after a disconnected client, not hang forever");
            const long long eof_after_ms = upstream.eof_observed_after_ms.load();
            std::cerr << "  scenario B: revlm closed upstream " << eof_after_ms
                      << "ms after the last upstream write (full idle timeout would be "
                      << (kUpstreamTimeoutSeconds * 1000) << "ms)\n";
            // The post-disconnect drain timeout (gateway.cpp's
            // kDisconnectDrainTimeoutMs) is 1500ms. Assert we gave up well
            // short of the full idle timeout, proving the short drain path
            // engaged instead of the core reading upstream indefinitely.
            expect(eof_after_ms >= 0 && eof_after_ms < (kUpstreamTimeoutSeconds * 1000) - 1500,
                   "core should stop reading a silent upstream within the short post-disconnect drain window");

            // One row, and nothing invented in it. The request did reach a
            // channel, so it leaves a record (CONTEXT 请求提交); what it must not
            // do is bill for usage the upstream never sent.
            const auto rows = wait_for_rows(*db,
                                            "SELECT usd,usage_details FROM requests "
                                            "WHERE request_id='disc-b-1' ORDER BY id DESC LIMIT 2",
                                            1);
            expect(rows.size() == 1, "a stream that reached a channel should still leave exactly one record");
            if (rows.size() == 1) {
                expect(std::stod(rows[0][0].value_or("0")) == 0.0,
                       "core must not charge for usage it never received from upstream");
                expect(rows[0][1].value_or("") == "{}", "core must not invent a usage payload the upstream never sent");
            }
        }

        harness.stop();
    } catch (const std::exception &err) {
        std::cerr << "streaming disconnect test failed: " << err.what() << '\n';
        return 1;
    }

    return g_failures == 0 ? 0 : 1;
}
