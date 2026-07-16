#include "auth/users.hpp"
#include "store/mysql_test_env.hpp"
#include "util/user_input.hpp"
#include "channels/channels.hpp"
#include "server/http_server.hpp"
#include "server/tokens.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"

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

    ~MockUpstreamServer()
    {
        join();
    }
};

struct HttpHarness {
    revlm::HttpServer server;
    std::atomic_bool running{ true };
    std::thread thread;

    HttpHarness()
        : server()
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
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping chat completions MySQL test\n";
        return 0;
    }

    try {
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);
        revlm::Config config;
        config.addr = "127.0.0.1:18081";
        config.db_dsn = dsn;
        config.session_secret = "tmp-session-secret";
        revlm::test::install_test_runtime(config);

        revlm::sql_exec(*db, "DELETE FROM requests");
        revlm::sql_exec(*db, "DELETE FROM channel_group_members");
        revlm::sql_exec(*db, "DELETE FROM channel_groups");
        revlm::sql_exec(*db, "DELETE FROM channels");
        revlm::sql_exec(*db, "DELETE FROM user_tokens");
        revlm::sql_exec(*db, "DELETE FROM session_bindings");
        revlm::sql_exec(*db, "DELETE FROM users");

        revlm::UserStore &user_store = revlm::UserStore::instance();
        revlm::User user_id_user = revlm::User("chat@example.com", "chat", revlm::hash_password("password"), "user");
        user_id_user.status = 1;
        const long long user_id = user_store.create_user(std::move(user_id_user));
        revlm::TokenStore &token_store = user_store.tokens();
        const std::string raw_token = "sk_tmp_g003_chat";
        const long long token_id = token_store.create_user_token(user_id, odb::nullable<std::string>{}, raw_token);

        revlm::ChannelStore &channel_store = revlm::ChannelStore::instance();
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
        if (!token_store.set_token_channel(user_id, token_id, channel_id)) {
            std::cerr << "bind token channel failed\n";
            return 1;
        }

        const std::string non_stream_body =
            "{\"model\":\"gpt-5.5\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
        const std::string non_stream_request =
            "POST /v1/chat/completions HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " + raw_token +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(non_stream_body.size()) +
            "\r\n\r\n" + non_stream_body;

        const std::string zero_balance_response = revlm::handle_http_request(non_stream_request, false, "2003001");
        if (expect(contains(zero_balance_response, "HTTP/1.1 402 Payment Required"),
                   "zero balance chat request should reject before upstream") != 0 ||
            expect(upstream_non_stream.captured_request.empty(),
                   "zero balance chat request should not proxy upstream") != 0) {
            std::cerr << zero_balance_response << '\n';
            return 1;
        }

        revlm::UserStore &users = revlm::UserStore::instance();
        revlm::User funded = users.get_user_by_id(user_id);
        funded.balance_usd = 10.0;
        (void)users.update_user(funded);

        const std::string non_stream_response = revlm::handle_http_request(non_stream_request, false, "2003002");
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

        const auto usage_rows = revlm::sql_query_rows(*db, "SELECT model,input_tokens,output_tokens,is_stream,status "
                                                           "FROM requests ORDER BY id DESC LIMIT 1");
        if (expect(!usage_rows.empty(), "non-stream request should write usage event") != 0 ||
            expect(usage_rows[0][0].value_or("") == "gpt-5.5", "usage model should match request model") != 0 ||
            expect(usage_rows[0][1].value_or("") == "12", "usage prompt tokens should be extracted") != 0 ||
            expect(usage_rows[0][2].value_or("") == "5", "usage completion tokens should be extracted") != 0 ||
            expect(usage_rows[0][3].value_or("") == "0", "non-stream request should record is_stream=0") != 0 ||
            expect(usage_rows[0][4].value_or("") == "committed", "non-stream chat usage should be committed") != 0) {
            return 1;
        }
        if (expect(users.get_user_balance_usd(user_id) != 10.0, "non-stream chat should debit user balance") != 0) {
            return 1;
        }

        revlm::sql_exec(*db, "DELETE FROM requests");
        openai_ch.base_url = "://bad-upstream";
        if (!channel_store.update_channel(openai_ch)) {
            std::cerr << "failed to update channel base_url\n";
            return 1;
        }
        config.gateway_max_retry_attempts = 1;
        config.gateway_max_failover_switches = 0;
        config.gateway_max_retry_elapsed_ms = 1000;
        revlm::reset_config_for_test(config);
        revlm::reset_config_for_test(config);
        const std::string parse_failure_response = revlm::handle_http_request(non_stream_request, false, "2003003");
        if (expect(contains(parse_failure_response, "HTTP/1.1 502 Bad Gateway"),
                   "invalid upstream should return bad gateway") != 0) {
            std::cerr << parse_failure_response << '\n';
            return 1;
        }

        const auto parse_failure_usage_rows =
            revlm::sql_query_rows(*db, "SELECT status_code,error_class,channel_id "
                                       "FROM requests WHERE request_id='2003003' ORDER BY id DESC LIMIT 1");
        if (expect(!parse_failure_usage_rows.empty(), "invalid upstream should still write usage event") != 0 ||
            expect(parse_failure_usage_rows[0][0].value_or("") == "502", "invalid upstream usage should record 502") !=
                0 ||
            expect(parse_failure_usage_rows[0][1].value_or("") == "invalid_upstream_url",
                   "invalid upstream usage should keep parse classification") != 0 ||
            expect(parse_failure_usage_rows[0][2].value_or("") == std::to_string(channel_id),
                   "invalid upstream usage should keep attempted channel id") != 0) {
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
        revlm::sql_exec(*db, "DELETE FROM requests");
        openai_ch.base_url = "http://127.0.0.1:" + std::to_string(upstream_stream.port);
        if (!channel_store.update_channel(openai_ch)) {
            std::cerr << "failed to update channel for stream test\n";
            return 1;
        }
        HttpHarness server;
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

        const auto stream_usage_rows = revlm::sql_query_rows(*db,
                                                             "SELECT input_tokens,output_tokens,is_stream,model,status "
                                                             "FROM requests ORDER BY id DESC LIMIT 1");
        if (expect(!stream_usage_rows.empty(), "stream request should write usage event") != 0 ||
            expect(stream_usage_rows[0][0].value_or("") == "8", "stream prompt tokens should be extracted") != 0 ||
            expect(stream_usage_rows[0][1].value_or("") == "4", "stream completion tokens should be extracted") != 0 ||
            expect(stream_usage_rows[0][2].value_or("") == "1", "stream request should record is_stream=1") != 0 ||
            expect(stream_usage_rows[0][3].value_or("") == "gpt-5.5", "stream model should be recorded") != 0 ||
            expect(stream_usage_rows[0][4].value_or("") == "committed", "stream chat usage should be committed") != 0) {
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
