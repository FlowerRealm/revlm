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
            pollfd pollfd{};
            pollfd.fd = listener;
            pollfd.events = POLLIN;
            if (::poll(&pollfd, 1, 5000) <= 0) {
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

} // namespace

int main()
{
    const char *dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping gateway failover MySQL test\n";
        return 0;
    }

    try {
        auto step = [](const char *name) { std::cerr << "[g008] " << name << '\n'; };
        step("migrate");
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);
        revlm::Config config;
        config.db_dsn = dsn;
        config.session_secret = "tmp-session-secret";
        revlm::test::install_test_runtime(config);

        step("seed");
        revlm::sql_exec(*db, "DELETE FROM requests");
        revlm::sql_exec(*db, "DELETE FROM channel_group_members");
        revlm::sql_exec(*db, "DELETE FROM channel_groups");
        revlm::sql_exec(*db, "DELETE FROM channels");
        revlm::sql_exec(*db, "DELETE FROM user_tokens");
        revlm::sql_exec(*db, "DELETE FROM session_bindings");
        revlm::sql_exec(*db, "DELETE FROM users");

        revlm::UserStore &user_store = revlm::UserStore::instance();
        revlm::TokenStore &token_store = user_store.tokens();
        revlm::User user("g008@example.com", "chato", revlm::hash_password("password"), "user");
        user.status = 1;
        const long long user_id = user_store.create_user(std::move(user));
        const std::string raw_token = "sk_tmp_g008_single";
        const long long token_id = token_store.create_user_token(user_id, odb::nullable<std::string>{}, raw_token);

        revlm::User funded = user_store.get_user_by_id(user_id);
        funded.balance_usd = 10.0;
        if (!user_store.update_user(funded)) {
            std::cerr << "failed to fund user\n";
            return 1;
        }

        MockUpstreamServer healthy_upstream;
        healthy_upstream.start("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                               "{\"id\":\"chatcmpl-ok\",\"object\":\"chat.completion\",\"model\":\"gpt-5.5\","
                               "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"ok\"}}],"
                               "\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3,\"total_tokens\":10}}");

        revlm::ChannelStore &channel_store = revlm::ChannelStore::instance();
        revlm::Channel channel;
        channel.type = 2;
        channel.name = "tmp-g008-channel";
        channel.priority = 10;
        channel.status = true;
        channel.base_url = "http://127.0.0.1:" + std::to_string(healthy_upstream.port);
        channel.api_key = "upstream-secret-1";
        if (!channel_store.create_channel(channel)) {
            std::cerr << "create channel failed\n";
            return 1;
        }
        const long long channel_id = channel.id;
        if (!token_store.set_token_channel(user_id, token_id, channel_id)) {
            std::cerr << "bind token channel failed\n";
            return 1;
        }

        config.gateway_max_retry_attempts = 1;
        config.gateway_max_failover_switches = 0;
        config.gateway_max_retry_elapsed_ms = 1000;
        revlm::reset_config_for_test(config);
        revlm::reset_config_for_test(config);

        const std::string body = "{\"model\":\"gpt-5.5\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
        const std::string request =
            "POST /v1/chat/completions HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " + raw_token +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

        step("success-request");
        const std::string success_response = revlm::handle_http_request(request, false, "2008001");
        healthy_upstream.join();
        step("success-assert");

        if (expect(contains(success_response, "HTTP/1.1 200 OK"), "single-channel request should succeed") != 0 ||
            expect(contains(healthy_upstream.captured_request, "Authorization: Bearer upstream-secret-1"),
                   "request should use bound channel key") != 0) {
            std::cerr << success_response << '\n';
            return 1;
        }

        const auto usage_rows = revlm::sql_query_rows(*db, "SELECT status_code,input_tokens,output_tokens,channel_id "
                                                           "FROM requests WHERE request_id='2008001' "
                                                           "ORDER BY id DESC LIMIT 1");
        if (expect(!usage_rows.empty(), "request should write usage event") != 0 ||
            expect(usage_rows[0][0].value_or("") == "200", "usage should record success status") != 0 ||
            expect(usage_rows[0][1].value_or("") == "7", "usage should record prompt tokens") != 0 ||
            expect(usage_rows[0][2].value_or("") == "3", "usage should record completion tokens") != 0 ||
            expect(usage_rows[0][3].value_or("") == std::to_string(channel_id),
                   "usage should point at bound channel") != 0) {
            return 1;
        }

        step("parse-request");
        revlm::sql_exec(*db, "DELETE FROM requests");
        channel.base_url = "://bad-upstream";
        if (!channel_store.update_channel(channel)) {
            std::cerr << "failed to update channel base_url\n";
            return 1;
        }
        const std::string parse_failure_response = revlm::handle_http_request(request, false, "2008002");
        step("parse-assert");
        if (expect(contains(parse_failure_response, "HTTP/1.1 502 Bad Gateway"),
                   "invalid upstream should return bad gateway") != 0) {
            std::cerr << parse_failure_response << '\n';
            return 1;
        }

        const auto parse_usage_rows =
            revlm::sql_query_rows(*db, "SELECT status_code,error_class,channel_id "
                                       "FROM requests WHERE request_id='2008002' ORDER BY id DESC LIMIT 1");
        if (expect(!parse_usage_rows.empty(), "parse failure should write usage event") != 0 ||
            expect(parse_usage_rows[0][0].value_or("") == "502", "parse failure usage should record 502") != 0 ||
            expect(parse_usage_rows[0][1].value_or("") == "invalid_upstream_url",
                   "parse failure usage should keep parse classification") != 0 ||
            expect(parse_usage_rows[0][2].value_or("") == std::to_string(channel_id),
                   "parse failure usage should keep attempted channel id") != 0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "gateway failover MySQL test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
