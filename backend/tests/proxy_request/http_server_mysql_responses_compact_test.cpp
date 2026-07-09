#include "auth/users.hpp"
#include "util/user_input.hpp"
#include "billing/billing.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "server/http_server.hpp"
#include "server/tokens.hpp"
#include "store/migrations.hpp"
#include "store/mysql.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace
{

using namespace std::chrono_literals;

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

struct MockGatewayServer {
    int port = 0;
    int listener = -1;
    std::string captured_request;
    std::thread thread;

    ~MockGatewayServer()
    {
        stop();
        join();
    }

    void start(std::function<void(int)> serve)
    {
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(listener, 1) != 0) {
            std::cerr << "mock gateway listen failed: " << std::strerror(errno) << '\n';
            std::exit(1);
        }
        socklen_t len = sizeof(addr);
        (void)::getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &len);
        port = ntohs(addr.sin_port);
        thread = std::thread([this, serve = std::move(serve)]() mutable {
            int client = ::accept(listener, nullptr, nullptr);
            ::close(listener);
            listener = -1;
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
            serve(client);
            ::close(client);
        });
    }

    void stop()
    {
        if (!thread.joinable()) {
            return;
        }
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
            ;
        ::close(fd);
    }

    void join()
    {
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

struct HttpHarness {
    revlm::HttpServer server;
    std::atomic_bool running{ true };
    std::thread thread;

    explicit HttpHarness(revlm::Config config)
        : server(std::move(config), revlm::BuildInfo{ "test-version", "test-date" })
    {
    }

    ~HttpHarness()
    {
        stop();
    }

    void start()
    {
        thread = std::thread([&] { server.run(running); });
    }

    void stop()
    {
        if (!thread.joinable()) {
            return;
        }
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

} // namespace

int main()
{
    const char *dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping responses compact MySQL test\n";
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

        revlm::UserStore &user_store = revlm::UserStore::instance();
        user_store.reload(conn);
        revlm::User user_id_user = revlm::User("compact@example.com", "compact", revlm::hash_password("password"),
                                               "user");
        user_id_user.status = 1;
        const long long user_id = user_store.create_user(std::move(user_id_user));
        revlm::TokenStore token_store(conn);
        const std::string raw_token = "sk_tmp_g005_compact";
        const long long token_id = token_store.create_user_token(user_id, std::nullopt, raw_token);

        revlm::ChannelGroupStore &group_store = revlm::ChannelGroupStore::instance();
        group_store.reload(conn);
        const long long group_id = group_store.create_channel_group("tmp_g005_group", "", 1.0);

        revlm::ChannelStore channel_store(conn);
        revlm::Channel openai_ch;
        openai_ch.type = 1;
        openai_ch.name = "tmp-g005-openai";
        openai_ch.status = true;
        openai_ch.base_url = "http://127.0.0.1:9";
        openai_ch.api_key = "unused-secret";
        if (!channel_store.create_channel(openai_ch)) {
            std::cerr << "create channel failed\n";
            return 1;
        }
        if (!group_store.add_channel_group_member(group_id, openai_ch)) {
            std::cerr << "bind channel group member failed\n";
            return 1;
        }
        if (!token_store.replace_token_channel_groups(token_id, { "tmp_g005_group" })) {
            std::cerr << "bind token groups failed\n";
            return 1;
        }
        if (!token_store.replace_token_model_mappings(token_id, { { "alias", "gpt-5.5" } })) {
            std::cerr << "bind token model mappings failed\n";
            return 1;
        }

        MockGatewayServer gateway_non_stream;
        gateway_non_stream.start([](int client) {
            const std::string response =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                "{\"id\":\"resp_ok\",\"model\":\"gpt-5.5\",\"usage\":{\"input_tokens\":8,\"output_tokens\":3,"
                "\"cache_read_input_tokens\":2,\"cache_creation_input_tokens\":5,"
                "\"cache_creation_1h_input_tokens\":7}}";
            (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL);
            ::shutdown(client, SHUT_WR);
        });

        revlm::Config config;
        config.addr = "127.0.0.1:18081";
        config.db_dsn = dsn;
        config.session_secret = "tmp-session-secret";
        config.compact_gateway_base_url = "http://127.0.0.1:" + std::to_string(gateway_non_stream.port);
        config.compact_gateway_key = "tmp-gateway-key";

        const std::string non_stream_body = "{\"model\":\"alias\",\"input\":\"hello\",\"session_id\":\"s1\"}";
        const std::string non_stream_request =
            "POST /v1/responses/compact HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " + raw_token +
            "\r\nContent-Type: application/json\r\nsession_id: s1h\r\noriginator: cli\r\nContent-Length: " +
            std::to_string(non_stream_body.size()) + "\r\n\r\n" + non_stream_body;

        const std::string zero_balance_response =
            revlm::handle_http_request(non_stream_request, config, revlm::BuildInfo{ "test-version", "test-date" },
                                       false, "2005001");
        if (expect(contains(zero_balance_response, "HTTP/1.1 402 Payment Required"),
                   "zero balance compact request should reject before upstream") != 0 ||
            expect(gateway_non_stream.captured_request.empty(),
                   "zero balance compact request should not proxy upstream") != 0) {
            std::cerr << zero_balance_response << '\n';
            return 1;
        }

        revlm::UserStore &users = revlm::UserStore::instance();
        users.reload(conn);
        revlm::User funded = users.get_user_by_id(user_id);
        funded.balance_usd = "10.000000";
        (void)users.update_user(funded);
        revlm::BillingStore billing(conn);

        const std::string non_stream_response = revlm::handle_http_request(
            non_stream_request, config, revlm::BuildInfo{ "test-version", "test-date" }, false, "2005002");
        gateway_non_stream.stop();
        gateway_non_stream.join();

        if (expect(contains(non_stream_response, "HTTP/1.1 200 OK"), "non-stream compact should succeed") != 0 ||
            expect(contains(non_stream_response, "\"resp_ok\""), "non-stream compact should proxy upstream json") !=
                0 ||
            expect(contains(gateway_non_stream.captured_request, "Authorization: Bearer tmp-gateway-key"),
                   "gateway request should use configured gateway key") != 0 ||
            expect(contains(gateway_non_stream.captured_request, "\"model\":\"gpt-5.5\""),
                   "gateway request should rewrite alias to bound upstream model") != 0 ||
            expect(!contains(gateway_non_stream.captured_request, "\"session_id\""),
                   "gateway request body should strip session_id") != 0) {
            std::cerr << non_stream_response << '\n' << gateway_non_stream.captured_request << '\n';
            return 1;
        }

        const auto usage_rows = conn.query_rows(
            "SELECT model,input_tokens,output_tokens,cache_read_tokens,cache_creation_5m_tokens,"
            "cache_creation_1h_tokens,is_stream,status "
            "FROM usage_events ORDER BY id DESC LIMIT 1");
        if (expect(!usage_rows.empty(), "non-stream compact should write usage event") != 0 ||
            expect(usage_rows[0][0].value_or("") == "gpt-5.5",
                   "usage model should match bound upstream model") != 0 ||
            expect(usage_rows[0][1].value_or("") == "8", "usage input tokens should be extracted") != 0 ||
            expect(usage_rows[0][2].value_or("") == "3", "usage output tokens should be extracted") != 0 ||
            expect(usage_rows[0][3].value_or("") == "2", "usage cache read tokens should be extracted") != 0 ||
            expect(usage_rows[0][4].value_or("") == "5", "usage cache creation tokens should be extracted") != 0 ||
            expect(usage_rows[0][5].value_or("") == "7", "usage cache creation 1h tokens should be extracted") != 0 ||
            expect(usage_rows[0][6].value_or("") == "0", "non-stream compact should record is_stream=0") != 0 ||
            expect(usage_rows[0][7].value_or("") == "committed",
                   "non-stream compact usage should be committed") != 0) {
            return 1;
        }
        if (expect(billing.get_user_balance_usd(user_id) != "10.000000",
                   "non-stream compact should debit user balance") != 0) {
            return 1;
        }

        MockGatewayServer gateway_4xx;
        gateway_4xx.start([](int client) {
            const std::string response =
                "HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                "{\"error\":{\"message\":\"rate limited\"}}";
            (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL);
            ::shutdown(client, SHUT_WR);
        });

        config.compact_gateway_base_url = "http://127.0.0.1:" + std::to_string(gateway_4xx.port);
        const std::string rate_limit_body = "{\"model\":\"alias\",\"input\":\"retry me\"}";
        const std::string rate_limit_request =
            "POST /v1/responses/compact HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " + raw_token +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(rate_limit_body.size()) +
            "\r\n\r\n" + rate_limit_body;
        const std::string rate_limit_response = revlm::handle_http_request(
            rate_limit_request, config, revlm::BuildInfo{ "test-version", "test-date" }, false, "2005003");
        gateway_4xx.stop();
        gateway_4xx.join();

        if (expect(contains(rate_limit_response, "HTTP/1.1 429 Upstream"),
                   "compact should preserve upstream 4xx status") != 0 ||
            expect(contains(rate_limit_response, "rate limited"), "compact should proxy upstream 4xx body") != 0) {
            std::cerr << rate_limit_response << '\n';
            return 1;
        }

        const auto rate_limit_usage_rows =
            conn.query_rows("SELECT model,is_stream,status_code FROM usage_events "
                            "WHERE id=2005003 ORDER BY id DESC LIMIT 1");
        if (expect(!rate_limit_usage_rows.empty(), "compact 4xx should still write usage event") != 0 ||
            expect(rate_limit_usage_rows[0][0].value_or("") == "gpt-5.5",
                   "compact 4xx usage should retain bound upstream model") != 0 ||
            expect(rate_limit_usage_rows[0][1].value_or("") == "0", "compact 4xx should record non-stream usage") !=
                0 ||
            expect(rate_limit_usage_rows[0][2].value_or("") == "429", "compact 4xx should record upstream status") !=
                0) {
            return 1;
        }

        MockGatewayServer gateway_stream;
        std::atomic_bool sent_first{ false };
        gateway_stream.start([&](int client) {
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
            const std::string first =
                "data: {\"type\":\"response.output_text.delta\",\"model\":\"gpt-5.5\",\"delta\":\"hi\"}\n\n";
            const std::string done =
                "data: {\"type\":\"response.completed\",\"response\":{\"model\":\"gpt-5.5\",\"usage\":{\"input_tokens\":9,\"output_tokens\":4}}}\n\n"
                "data: [DONE]\n\n";
            (void)::send(client, head.data(), head.size(), MSG_NOSIGNAL);
            std::this_thread::sleep_for(25ms);
            (void)::send(client, first.data(), first.size(), MSG_NOSIGNAL);
            sent_first.store(true);
            std::this_thread::sleep_for(140ms);
            (void)::send(client, done.data(), done.size(), MSG_NOSIGNAL);
            ::shutdown(client, SHUT_WR);
        });

        config.compact_gateway_base_url = "http://127.0.0.1:" + std::to_string(gateway_stream.port);
        HttpHarness harness(config);
        harness.start();
        std::this_thread::sleep_for(50ms);

        const std::string stream_body =
            "{\"model\":\"alias\",\"input\":\"hello\",\"session_id\":\"s1\",\"stream\":true}";
        const std::string stream_request =
            "POST /v1/responses/compact HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " + raw_token +
            "\r\nContent-Type: application/json\r\nsession_id: s1h\r\nContent-Length: " +
            std::to_string(stream_body.size()) + "\r\n\r\n" + stream_body;

        const auto started = std::chrono::steady_clock::now();
        const std::string stream_response = send_http_request(stream_request);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        harness.stop();
        gateway_stream.stop();
        gateway_stream.join();

        if (expect(contains(stream_response, "HTTP/1.1 200 OK"), "stream compact should succeed") != 0 ||
            expect(contains(stream_response, "response.output_text.delta"),
                   "stream compact should flush delta to client") != 0 ||
            expect(sent_first.load(), "mock gateway should send first chunk") != 0 ||
            expect(elapsed.count() < 300, "stream compact should not wait excessively before returning") != 0) {
            std::cerr << stream_response << '\n';
            return 1;
        }

        const auto stream_usage_rows =
            conn.query_rows("SELECT model,input_tokens,output_tokens,is_stream,status "
                            "FROM usage_events WHERE is_stream=1 ORDER BY id DESC LIMIT 1");
        if (expect(!stream_usage_rows.empty(), "stream compact should write usage event") != 0 ||
            expect(stream_usage_rows[0][0].value_or("") == "gpt-5.5",
                   "stream usage model should match bound model") != 0 ||
            expect(stream_usage_rows[0][1].value_or("") == "9", "stream usage input tokens should be extracted") != 0 ||
            expect(stream_usage_rows[0][2].value_or("") == "4", "stream usage output tokens should be extracted") !=
                0 ||
            expect(stream_usage_rows[0][3].value_or("") == "1", "stream compact should record is_stream=1") != 0 ||
            expect(stream_usage_rows[0][4].value_or("") == "committed",
                   "stream compact usage should be committed") != 0) {
            return 1;
        }

    } catch (const std::exception &err) {
        std::cerr << "responses compact test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
