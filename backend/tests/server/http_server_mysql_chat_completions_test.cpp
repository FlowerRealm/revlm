#include "auth/users.hpp"
#include "billing/billing.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "server/http_server.hpp"
#include "server/tokens.hpp"
#include "store/migrations.hpp"
#include "store/mysql.hpp"

#include <openssl/sha.h>

#include <array>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

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

std::string sha256_hex(std::string_view value)
{
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), digest.data());
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        out.push_back(hex[(byte >> 4U) & 0x0fU]);
        out.push_back(hex[byte & 0x0fU]);
    }
    return out;
}

int sequential_route_start_index(std::string_view route_key_hash, int count)
{
    if (count <= 1) {
        return 0;
    }
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char *>(route_key_hash.data()), route_key_hash.size(), digest.data());
    std::uint64_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
        value = (value << 8U) | digest[i];
    }
    return static_cast<int>(value % static_cast<std::uint64_t>(count));
}

int sequential_start_index_for_failover(long long user_id, long long token_id, std::string_view model, int count)
{
    const std::string route_key = std::to_string(user_id) + ":" + std::to_string(token_id) + ":" + std::string(model);
    return sequential_route_start_index(sha256_hex(route_key), count);
}

std::string recv_until_close(int fd)
{
    std::string out;
    char buffer[4096];
    while (true) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        out.append(buffer, static_cast<size_t>(n));
    }
    return out;
}

struct MockUpstreamServer {
    int port = 0;
    std::string captured_request;
    std::thread thread;

    void start(std::string response)
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
        thread = std::thread([this, listener, response = std::move(response)]() mutable {
            pollfd pfd{};
            pfd.fd = listener;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 5000) <= 0) {
                ::close(listener);
                return;
            }
            int client = ::accept(listener, nullptr, nullptr);
            ::close(listener);
            char buffer[4096];
            while (client >= 0) {
                const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
                if (n <= 0) {
                    break;
                }
                captured_request.append(buffer, static_cast<size_t>(n));
                if (captured_request.find("\r\n\r\n") != std::string::npos) {
                    const auto head_end = captured_request.find("\r\n\r\n");
                    const std::string marker = "Content-Length:";
                    const auto pos = captured_request.find(marker);
                    if (pos != std::string::npos) {
                        const auto line_end = captured_request.find("\r\n", pos);
                        const std::string value =
                            captured_request.substr(pos + marker.size(), line_end - pos - marker.size());
                        const size_t body_len = static_cast<size_t>(std::stoll(value));
                        if (captured_request.size() >= head_end + 4 + body_len) {
                            break;
                        }
                    }
                }
            }
            if (client >= 0) {
                (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL);
                ::shutdown(client, SHUT_WR);
                ::close(client);
            }
        });
    }

    void join()
    {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

struct HttpHarness {
    revlm::HttpServer server;
    std::atomic_bool running{ true };
    std::thread thread;

    explicit HttpHarness(revlm::Config config)
        : server(std::move(config), revlm::BuildInfo{ "test-version", "test-date" })
    {
    }

    void start()
    {
        thread = std::thread([&] { server.run(running); });
    }

    void stop()
    {
        running.store(false);
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(18081);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
            ;
        ::close(fd);
        if (thread.joinable()) {
            thread.join();
        }
    }
};

std::string send_http_request(std::string_view request)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(18081);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        std::cerr << "connect failed: " << std::strerror(errno) << '\n';
        std::exit(1);
    }
    (void)::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    ::shutdown(fd, SHUT_WR);
    std::string response = recv_until_close(fd);
    ::close(fd);
    return response;
}

} // namespace

int main()
{
    const char *dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping chat completions MySQL test\n";
        return 0;
    }

    try {
        (void)revlm::apply_migrations(dsn, "internal/store/migrations", "", 30);

        revlm::MysqlConnection conn(dsn);
        conn.exec("DELETE FROM usage_events");
        conn.exec("DELETE FROM channel_group_members");
        conn.exec("DELETE FROM token_model_mappings");
        conn.exec("DELETE FROM token_channel_groups");
        conn.exec("DELETE FROM channel_groups");
        conn.exec("DELETE FROM channels");
        conn.exec("DELETE FROM user_tokens");
        conn.exec("DELETE FROM session_bindings");
        conn.exec("DELETE FROM user_balances");
        conn.exec("DELETE FROM users");

        revlm::UserStore user_store(conn);
        const long long user_id = user_store.create_user(
            revlm::CreateUserInput{ "chat@example.com", "chat", revlm::hash_password("password"), "user" });
        revlm::TokenStore token_store(conn);
        const std::string raw_token = "sk_tmp_g003_chat";
        const long long token_id = token_store.create_user_token(user_id, std::nullopt, raw_token);

        revlm::ChannelGroupStore &group_store = revlm::ChannelGroupStore::instance();
        group_store.reload(conn);
        const long long group_id = group_store.create_channel_group("tmp_g003_openai", "", 1.0);
        if (!token_store.replace_token_channel_groups(token_id, { "tmp_g003_openai" })) {
            std::cerr << "bind token groups failed\n";
            return 1;
        }

        revlm::ChannelStore channel_store(conn);
        MockUpstreamServer upstream_non_stream;
        upstream_non_stream.start("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                                  "{\"id\":\"chatcmpl-test\",\"object\":\"chat.completion\",\"model\":\"gpt-5.5\","
                                  "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"hi\"}}],"
                                  "\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":5,\"total_tokens\":17}}");

        revlm::Channel openai_ch;
        openai_ch.type = 2;
        openai_ch.name = "tmp-g003-openai";
        openai_ch.priority = 10;
        openai_ch.status = true;
        openai_ch.base_url = "http://127.0.0.1:" + std::to_string(upstream_non_stream.port);
        openai_ch.api_key = "upstream-secret";
        if (!channel_store.create_channel(openai_ch)) {
            std::cerr << "create channel failed\n";
            return 1;
        }
        const long long channel_id = openai_ch.id;
        if (!group_store.add_channel_group_member(group_id, openai_ch)) {
            std::cerr << "bind channel group member failed\n";
            return 1;
        }

        revlm::Config config;
        config.addr = "127.0.0.1:18081";
        config.db_dsn = dsn;
        config.session_secret = "tmp-session-secret";

        const std::string non_stream_body =
            "{\"model\":\"gpt-5.5\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
        const std::string non_stream_request =
            "POST /v1/chat/completions HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " + raw_token +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(non_stream_body.size()) +
            "\r\n\r\n" + non_stream_body;

        const std::string zero_balance_response =
            revlm::handle_http_request(non_stream_request, config, revlm::BuildInfo{ "test-version", "test-date" },
                                       false, "req-g003-zero-balance");
        if (expect(contains(zero_balance_response, "HTTP/1.1 402 Payment Required"),
                   "zero balance chat request should reject before upstream") != 0 ||
            expect(upstream_non_stream.captured_request.empty(),
                   "zero balance chat request should not proxy upstream") != 0) {
            std::cerr << zero_balance_response << '\n';
            return 1;
        }

        revlm::UserStore users(conn);
        (void)users.add_user_balance_usd(user_id, "10.000000");
        revlm::BillingStore billing(conn);

        const std::string non_stream_response = revlm::handle_http_request(
            non_stream_request, config, revlm::BuildInfo{ "test-version", "test-date" }, false, "req-g003-nonstream");
        upstream_non_stream.join();
        if (expect(contains(non_stream_response, "HTTP/1.1 200 OK"), "non-stream chat completions should succeed") !=
                0 ||
            expect(contains(non_stream_response, "\"chat.completion\""),
                   "non-stream response should proxy upstream json") != 0 ||
            expect(contains(upstream_non_stream.captured_request, "Authorization: Bearer upstream-secret"),
                   "upstream request should use channel api key") != 0) {
            std::cerr << non_stream_response << '\n' << upstream_non_stream.captured_request << '\n';
            return 1;
        }

        const auto usage_rows =
            conn.query_rows("SELECT model,forwarded_model,upstream_response_model,input_tokens,output_tokens,is_stream,"
                            "committed_usd "
                            "FROM usage_events ORDER BY id DESC LIMIT 1");
        if (expect(!usage_rows.empty(), "non-stream request should write usage event") != 0 ||
            expect(usage_rows[0][0].value_or("") == "gpt-5.5", "usage model should match request model") != 0 ||
            expect(usage_rows[0][1].value_or("") == "gpt-5.5", "usage forwarded model should match upstream") != 0 ||
            expect(usage_rows[0][2].value_or("") == "gpt-5.5", "usage upstream model should be extracted") != 0 ||
            expect(usage_rows[0][3].value_or("") == "12", "usage prompt tokens should be extracted") != 0 ||
            expect(usage_rows[0][4].value_or("") == "5", "usage completion tokens should be extracted") != 0 ||
            expect(usage_rows[0][5].value_or("") == "0", "non-stream request should record is_stream=0") != 0 ||
            expect(usage_rows[0][6].value_or("") != "0.000000", "non-stream chat usage should record committed_usd") !=
                0) {
            return 1;
        }
        if (expect(billing.get_user_balance_usd(user_id) != "10.000000", "non-stream chat should debit user balance") !=
            0) {
            return 1;
        }

        conn.exec("DELETE FROM usage_events");
        openai_ch.base_url = "://bad-upstream";
        if (!channel_store.update_channel(openai_ch)) {
            std::cerr << "failed to update channel base_url\n";
            return 1;
        }
        config.gateway_max_retry_attempts = 1;
        config.gateway_max_failover_switches = 0;
        config.gateway_max_retry_elapsed_ms = 1000;
        const std::string parse_failure_response =
            revlm::handle_http_request(non_stream_request, config, revlm::BuildInfo{ "test-version", "test-date" },
                                       false, "req-g008-parse-failure");
        if (expect(contains(parse_failure_response, "HTTP/1.1 502 Bad Gateway"),
                   "invalid upstream should return bad gateway") != 0) {
            std::cerr << parse_failure_response << '\n';
            return 1;
        }

        const auto parse_failure_usage_rows =
            conn.query_rows("SELECT status_code,error_class,channel_id "
                            "FROM usage_events WHERE request_id='req-g008-parse-failure' ORDER BY id DESC LIMIT 1");
        if (expect(!parse_failure_usage_rows.empty(), "invalid upstream should still write usage event") != 0 ||
            expect(parse_failure_usage_rows[0][0].value_or("") == "502", "invalid upstream usage should record 502") !=
                0 ||
            expect(parse_failure_usage_rows[0][1].value_or("") == "invalid_upstream_url",
                   "invalid upstream usage should keep parse classification") != 0 ||
            expect(parse_failure_usage_rows[0][2].value_or("") == std::to_string(channel_id),
                   "invalid upstream usage should keep attempted channel id") != 0) {
            return 1;
        }

        MockUpstreamServer failover_first_upstream;
        failover_first_upstream.start(
            "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
            "{\"error\":{\"message\":\"temporary\",\"type\":\"server_error\"}}");
        MockUpstreamServer failover_second_upstream;
        failover_second_upstream.start(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
            "{\"id\":\"chatcmpl-failover\",\"object\":\"chat.completion\",\"model\":\"gpt-5.5\","
            "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"recovered\"}}],"
            "\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3,\"total_tokens\":10}}");
        conn.exec("DELETE FROM usage_events");
        openai_ch.base_url = "http://127.0.0.1:" + std::to_string(failover_first_upstream.port);
        if (!channel_store.update_channel(openai_ch)) {
            std::cerr << "failed to update primary channel for failover\n";
            return 1;
        }
        revlm::Channel failover_ch;
        failover_ch.type = 2;
        failover_ch.name = "tmp-g008-failover-openai";
        failover_ch.priority = 1;
        failover_ch.status = true;
        failover_ch.base_url = "http://127.0.0.1:" + std::to_string(failover_second_upstream.port);
        failover_ch.api_key = "upstream-secret-2";
        if (!channel_store.create_channel(failover_ch)) {
            std::cerr << "create failover channel failed\n";
            return 1;
        }
        const long long failover_channel_id = failover_ch.id;
        if (!group_store.add_channel_group_member(group_id, failover_ch)) {
            std::cerr << "bind failover channel group member failed\n";
            return 1;
        }
        std::string raw_failover_token;
        long long failover_token_id = 0;
        for (int i = 0; i < 8; ++i) {
            const std::string candidate = "sk_tmp_g008_failover_" + std::to_string(i);
            const long long candidate_token_id = token_store.create_user_token(user_id, std::nullopt, candidate);
            if (!token_store.replace_token_channel_groups(candidate_token_id, { "tmp_g003_openai" })) {
                std::cerr << "bind failover token groups failed\n";
                return 1;
            }
            if (sequential_start_index_for_failover(user_id, candidate_token_id, "gpt-5.5", 2) == 0) {
                raw_failover_token = candidate;
                failover_token_id = candidate_token_id;
                break;
            }
        }
        if (raw_failover_token.empty() || failover_token_id <= 0) {
            std::cerr << "failed to find deterministic failover token\n";
            return 1;
        }
        config.gateway_max_retry_attempts = 2;
        config.gateway_max_failover_switches = 1;
        config.gateway_max_retry_elapsed_ms = 1000;
        const std::string failover_request =
            "POST /v1/chat/completions HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " + raw_failover_token +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(non_stream_body.size()) +
            "\r\n\r\n" + non_stream_body;
        const std::string failover_response = revlm::handle_http_request(
            failover_request, config, revlm::BuildInfo{ "test-version", "test-date" }, false, "req-g003-failover");
        failover_first_upstream.join();
        failover_second_upstream.join();
        if (expect(contains(failover_response, "HTTP/1.1 200 OK"),
                   "failover request should recover on second attempt") != 0 ||
            expect(!failover_first_upstream.captured_request.empty(), "failover should try the first upstream") != 0 ||
            expect(!failover_second_upstream.captured_request.empty(),
                   "failover should rotate to the second upstream") != 0 ||
            expect(contains(failover_first_upstream.captured_request, "Authorization: Bearer upstream-secret"),
                   "first failover attempt should use primary channel key") != 0 ||
            expect(contains(failover_second_upstream.captured_request, "Authorization: Bearer upstream-secret-2"),
                   "second failover attempt should use rotated channel key") != 0 ||
            expect(contains(failover_response, "\"recovered\""),
                   "failover response should come from rotated upstream") != 0) {
            std::cerr << failover_response << '\n';
            return 1;
        }

        const auto failover_usage_rows =
            conn.query_rows("SELECT status_code,input_tokens,output_tokens,channel_id "
                            "FROM usage_events "
                            "WHERE request_id='req-g003-failover' ORDER BY id DESC LIMIT 1");
        if (expect(!failover_usage_rows.empty(), "failover request should still write usage event") != 0 ||
            expect(failover_usage_rows[0][0].value_or("") == "200",
                   "failover usage should record final success status") != 0 ||
            expect(failover_usage_rows[0][1].value_or("") == "7", "failover usage should use winning prompt tokens") !=
                0 ||
            expect(failover_usage_rows[0][2].value_or("") == "3",
                   "failover usage should use winning completion tokens") != 0 ||
            expect(failover_usage_rows[0][3].value_or("") == std::to_string(failover_channel_id),
                   "failover usage should record rotated winning channel") != 0) {
            return 1;
        }
        if (!group_store.remove_channel_group_member(group_id, failover_channel_id) ||
            !channel_store.delete_channel(failover_ch)) {
            std::cerr << "cleanup failover channel failed\n";
            return 1;
        }

        MockUpstreamServer upstream_stream;
        upstream_stream.start(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
            "data: {\"id\":\"chatcmpl-stream\",\"object\":\"chat.completion.chunk\",\"model\":\"gpt-5.5\","
            "\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
            "data: {\"id\":\"chatcmpl-stream\",\"object\":\"chat.completion.chunk\",\"model\":\"gpt-5.5\","
            "\"usage\":{\"prompt_tokens\":8,\"completion_tokens\":4,\"total_tokens\":12},\"choices\":[]}\n\n"
            "data: [DONE]\n\n");
        conn.exec("DELETE FROM usage_events");
        openai_ch.base_url = "http://127.0.0.1:" + std::to_string(upstream_stream.port);
        if (!channel_store.update_channel(openai_ch)) {
            std::cerr << "failed to update channel for stream test\n";
            return 1;
        }
        HttpHarness server(config);
        server.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const std::string stream_body =
            "{\"model\":\"gpt-5.5\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
        const std::string stream_request =
            "POST /v1/chat/completions HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " + raw_token +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(stream_body.size()) + "\r\n\r\n" +
            stream_body;
        const std::string stream_response = send_http_request(stream_request);
        upstream_stream.join();
        server.stop();

        if (expect(contains(stream_response, "HTTP/1.1 200 OK"), "stream chat completions should succeed") != 0 ||
            expect(contains(stream_response, "text/event-stream"),
                   "stream response should preserve SSE content type") != 0 ||
            expect(contains(stream_response, "data: [DONE]"), "stream response should proxy done marker") != 0) {
            std::cerr << stream_response << '\n';
            server.stop();
            return 1;
        }

        const auto stream_usage_rows =
            conn.query_rows("SELECT input_tokens,output_tokens,is_stream,upstream_response_model,committed_usd "
                            "FROM usage_events ORDER BY id DESC LIMIT 1");
        if (expect(!stream_usage_rows.empty(), "stream request should write usage event") != 0 ||
            expect(stream_usage_rows[0][0].value_or("") == "8", "stream prompt tokens should be extracted") != 0 ||
            expect(stream_usage_rows[0][1].value_or("") == "4", "stream completion tokens should be extracted") != 0 ||
            expect(stream_usage_rows[0][2].value_or("") == "1", "stream request should record is_stream=1") != 0 ||
            expect(stream_usage_rows[0][3].value_or("") == "gpt-5.5", "stream upstream model should be extracted") !=
                0 ||
            expect(stream_usage_rows[0][4].value_or("") != "0.000000",
                   "stream chat usage should record committed_usd") != 0) {
            server.stop();
            return 1;
        }
        server.stop();
    } catch (const std::exception &err) {
        std::cerr << "chat completions MySQL test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
