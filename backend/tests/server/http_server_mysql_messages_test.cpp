#include "auth/users.hpp"
#include "util/user_input.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "server/http_server.hpp"
#include "server/tokens.hpp"
#include "store/database.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
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
            int client = ::accept(listener, nullptr, nullptr);
            ::close(listener);
            char buffer[4096];
            while (true) {
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
            (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL);
            ::shutdown(client, SHUT_WR);
            ::close(client);
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
    // Do not SHUT_WR: httplib treats peer FIN as dead and skips chunked providers.
    std::string response = recv_until_close(fd);
    ::close(fd);
    return response;
}

} // namespace

int main()
{
    const char *dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping messages MySQL test\n";
        return 0;
    }

    try {
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);

        revlm::sql_exec(*db, "DELETE FROM requests");
        revlm::sql_exec(*db, "DELETE FROM channel_group_members");
        revlm::sql_exec(*db, "DELETE FROM token_model_mappings");
        revlm::sql_exec(*db, "DELETE FROM token_channel_groups");
        revlm::sql_exec(*db, "DELETE FROM channel_groups");
        revlm::sql_exec(*db, "DELETE FROM channels");
        revlm::sql_exec(*db, "DELETE FROM user_tokens");
        revlm::sql_exec(*db, "DELETE FROM session_bindings");
        revlm::sql_exec(*db, "DELETE FROM users");

        revlm::UserStore user_store(*db);
        revlm::User user_id_user =
            revlm::User("messages@example.com", "messages", revlm::hash_password("password"), "user");
        user_id_user.status = 1;
        const long long user_id = user_store.create_user(std::move(user_id_user));
        revlm::TokenStore &token_store = user_store.tokens();
        const std::string raw_token = "sk_tmp_g004_messages";
        const long long token_id = token_store.create_user_token(user_id, odb::nullable<std::string>{}, raw_token);

        revlm::ChannelGroupStore group_store(*db);
        const long long group_id = group_store.create_channel_group("tmp_g004_anthropic", "", 1.0);
        if (!token_store.replace_token_channel_groups(token_id, { "tmp_g004_anthropic" })) {
            std::cerr << "bind token groups failed\n";
            return 1;
        }

        revlm::ChannelStore channel_store(*db);
        MockUpstreamServer upstream_non_stream;
        upstream_non_stream.start("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                                  "{\"id\":\"msg_test\",\"type\":\"message\",\"role\":\"assistant\","
                                  "\"model\":\"claude-sonnet-4-6\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}],"
                                  "\"usage\":{\"input_tokens\":11,\"output_tokens\":7}}");

        revlm::Channel anthropic_ch;
        anthropic_ch.type = 4;
        anthropic_ch.name = "tmp-g004-anthropic";
        anthropic_ch.status = 1;
        anthropic_ch.base_url = "http://127.0.0.1:" + std::to_string(upstream_non_stream.port);
        anthropic_ch.api_key = "upstream-anthropic-secret";
        if (!channel_store.create_channel(anthropic_ch)) {
            std::cerr << "create channel failed\n";
            return 1;
        }
        if (!group_store.add_channel_group_member(group_id, anthropic_ch)) {
            std::cerr << "bind channel group member failed\n";
            return 1;
        }

        revlm::Config config;
        config.addr = "127.0.0.1:18081";
        config.db_dsn = dsn;
        config.session_secret = "tmp-session-secret";

        const std::string non_stream_body = "{\"model\":\"claude-sonnet-4-6\",\"max_tokens\":64,"
                                            "\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
        const std::string non_stream_request = "POST /v1/messages HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " +
                                               raw_token + "\r\nContent-Type: application/json\r\nContent-Length: " +
                                               std::to_string(non_stream_body.size()) + "\r\n\r\n" + non_stream_body;

        const std::string zero_balance_response = revlm::handle_http_request(
            non_stream_request, config, revlm::BuildInfo{ "test-version", "test-date" }, false, "2004001");
        if (expect(contains(zero_balance_response, "HTTP/1.1 402 Payment Required"),
                   "zero balance messages request should reject before upstream") != 0 ||
            expect(upstream_non_stream.captured_request.empty(),
                   "zero balance messages request should not proxy upstream") != 0) {
            std::cerr << zero_balance_response << '\n';
            return 1;
        }

        revlm::UserStore users(*db);
        revlm::User funded = users.get_user_by_id(user_id);
        funded.balance_usd = 10.0;
        (void)users.update_user(funded);

        const std::string non_stream_response = revlm::handle_http_request(
            non_stream_request, config, revlm::BuildInfo{ "test-version", "test-date" }, false, "2004002");
        upstream_non_stream.join();
        if (expect(contains(non_stream_response, "HTTP/1.1 200 OK"), "non-stream messages should succeed") != 0 ||
            expect(contains(non_stream_response, "\"type\":\"message\""),
                   "non-stream response should proxy upstream json") != 0 ||
            expect(contains(upstream_non_stream.captured_request, "x-api-key: upstream-anthropic-secret"),
                   "upstream request should use anthropic x-api-key") != 0 ||
            expect(contains(upstream_non_stream.captured_request, "anthropic-version: 2023-06-01"),
                   "upstream request should inject default anthropic version") != 0) {
            std::cerr << non_stream_response << '\n' << upstream_non_stream.captured_request << '\n';
            return 1;
        }

        const auto usage_rows = revlm::sql_query_rows(*db, "SELECT model,input_tokens,output_tokens,is_stream,status "
                                                           "FROM requests ORDER BY id DESC LIMIT 1");
        if (expect(!usage_rows.empty(), "non-stream request should write usage event") != 0 ||
            expect(usage_rows[0][0].value_or("") == "claude-sonnet-4-6", "usage model should match request model") !=
                0 ||
            expect(usage_rows[0][1].value_or("") == "11", "usage input tokens should be extracted") != 0 ||
            expect(usage_rows[0][2].value_or("") == "7", "usage output tokens should be extracted") != 0 ||
            expect(usage_rows[0][3].value_or("") == "0", "non-stream request should record is_stream=0") != 0 ||
            expect(usage_rows[0][4].value_or("") == "committed", "non-stream messages usage should be committed") !=
                0) {
            return 1;
        }
        if (expect(users.get_user_balance_usd(user_id) != 10.0,
                   "non-stream messages should debit user balance") != 0) {
            return 1;
        }

        MockUpstreamServer upstream_stream;
        upstream_stream.start(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_stream\",\"type\":\"message\","
            "\"role\":\"assistant\",\"model\":\"claude-sonnet-4-6\",\"usage\":{\"input_tokens\":9}}}\n\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
            "\"usage\":{\"output_tokens\":4}}\n\n"
            "data: {\"type\":\"message_stop\"}\n\n");
        revlm::sql_exec(*db, "DELETE FROM requests");
        anthropic_ch.base_url = "http://127.0.0.1:" + std::to_string(upstream_stream.port);
        if (!channel_store.update_channel(anthropic_ch)) {
            std::cerr << "failed to update channel for stream test\n";
            return 1;
        }
        HttpHarness server(config);
        server.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const std::string stream_body = "{\"model\":\"claude-sonnet-4-6\",\"stream\":true,\"max_tokens\":64,"
                                        "\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
        const std::string stream_request = "POST /v1/messages HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " +
                                           raw_token + "\r\nContent-Type: application/json\r\nContent-Length: " +
                                           std::to_string(stream_body.size()) + "\r\n\r\n" + stream_body;
        const std::string stream_response = send_http_request(stream_request);
        upstream_stream.join();
        server.stop();

        if (expect(contains(stream_response, "HTTP/1.1 200 OK"), "stream messages should succeed") != 0 ||
            expect(contains(stream_response, "text/event-stream"),
                   "stream response should preserve SSE content type") != 0 ||
            expect(contains(stream_response, "\"type\":\"message_stop\""),
                   "stream response should proxy anthropic SSE payloads") != 0) {
            std::cerr << stream_response << '\n';
            return 1;
        }

        const auto stream_usage_rows = revlm::sql_query_rows(*db,
                                                             "SELECT input_tokens,output_tokens,is_stream,model,status "
                                                             "FROM requests ORDER BY id DESC LIMIT 1");
        if (expect(!stream_usage_rows.empty(), "stream request should write usage event") != 0 ||
            expect(stream_usage_rows[0][0].value_or("") == "9", "stream input tokens should be extracted") != 0 ||
            expect(stream_usage_rows[0][1].value_or("") == "4", "stream output tokens should be extracted") != 0 ||
            expect(stream_usage_rows[0][2].value_or("") == "1", "stream request should record is_stream=1") != 0 ||
            expect(stream_usage_rows[0][3].value_or("") == "claude-sonnet-4-6", "stream model should be recorded") !=
                0 ||
            expect(stream_usage_rows[0][4].value_or("") == "committed", "stream messages usage should be committed") !=
                0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "messages MySQL test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
