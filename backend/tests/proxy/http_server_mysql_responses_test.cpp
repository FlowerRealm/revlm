#include "config/config.hpp"
#include "request/request.hpp"
#include "users/users.hpp"
#include "store/mysql_test_env.hpp"
#include "util/user_input.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "proxy/openai_responses.hpp"
#include "server/http_server.hpp"
#include "users/tokens.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <exception>
#include <httplib.h>
#include <netinet/in.h>
#include <odb/nullable.hxx>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
    std::atomic_bool ready{ false };

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
            (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL);
            ::shutdown(client, SHUT_WR);
            ::close(client);
        });
        for (int i = 0; i < 100 && !ready.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void join()
    {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

std::string api_request(std::string_view target, std::string_view token, std::string_view body,
                        std::string_view request_id)
{
    std::string req = "POST " + std::string(target) + " HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " +
                      std::string(token) +
                      "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                      "\r\n\r\n" + std::string(body);
    return revlm::handle_http_request(req, false, request_id);
}

} // namespace

int main()
{
    const char *dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping responses MySQL test\n";
        return 0;
    }

    try {
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);
        {
            revlm::Config __runtime_cfg;
            __runtime_cfg.db_dsn = dsn;
            revlm::test::install_test_runtime(__runtime_cfg);
        }

        const std::string suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

        revlm::sql_exec(*db, "DELETE FROM requests");
        revlm::sql_exec(*db, "DELETE FROM channel_group_members");
        revlm::sql_exec(*db, "DELETE FROM channel_groups");
        revlm::sql_exec(*db, "DELETE FROM channels");
        revlm::sql_exec(*db, "DELETE FROM user_tokens");
        revlm::sql_exec(*db, "DELETE FROM sessions");
        revlm::sql_exec(*db, "DELETE FROM users");

        revlm::UserStore &user_store = revlm::UserStore::instance();
        revlm::User user("responses" + suffix + "@example.com", "responses" + suffix, revlm::hash_password("password"),
                         "user");
        user.status = 1;
        const long long user_id = user_store.create_user(std::move(user));
        revlm::User funded = user_store.get_user_by_id(user_id);
        funded.balance_usd = 10.0;
        if (!user_store.update_user(funded)) {
            std::cerr << "failed to fund responses user\n";
            return 1;
        }

        revlm::TokenStore &token_store = user_store.tokens();
        const std::string raw_token = "sk_tmp_g002_responses_" + suffix;
        const long long token_id = token_store.create_user_token(user_id, odb::nullable<std::string>{}, raw_token);

        revlm::ChannelStore &channel_store = revlm::ChannelStore::instance();
        MockUpstreamServer upstream_ok;
        upstream_ok.start("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                          "{\"id\":\"resp_mock_1\",\"object\":\"response\",\"model\":\"gpt-5.5\","
                          "\"service_tier\":\"priority\",\"usage\":{\"input_tokens\":7,\"output_tokens\":3,"
                          "\"total_tokens\":10,\"cache_read_input_tokens\":2,\"cache_creation_input_tokens\":1}}");

        revlm::Channel success_ch(0, "openai_compatible", "tmp-g002-openai-success-" + suffix, true, 20,
                                  "http://127.0.0.1:" + std::to_string(upstream_ok.port), "sk-upstream-ok");
        if (!channel_store.create_channel(success_ch)) {
            std::cerr << "failed to create success channel\n";
            return 1;
        }
        const long long success_channel_id = success_ch.id;
        revlm::ChannelGroupStore &group_store = revlm::ChannelGroupStore::instance();
        const int group_id = group_store.create_channel_group("tmp-g002-group-" + suffix, "", 1.0, true);
        if (!group_store.add_channel_group_member(group_id, success_ch)) {
            std::cerr << "failed to add channel group member\n";
            return 1;
        }
        if (!token_store.set_token_channel_group(user_id, token_id, group_id)) {
            std::cerr << "failed to bind token channel group\n";
            return 1;
        }

        const std::string ok_body = "{\"model\":\"gpt-5.5\",\"input\":\"hello\",\"service_tier\":\"priority\"}";
        std::cerr << "[responses-test] non-stream\n";
        const std::string ok = api_request("/v1/responses", raw_token, ok_body, "2002001");
        upstream_ok.join();
        if (expect(contains(ok, "HTTP/1.1 200 OK"), "responses request should succeed") != 0 ||
            expect(contains(ok, "\"service_tier\":\"priority\""),
                   "responses body should pass through upstream service tier") != 0 ||
            expect(contains(upstream_ok.captured_request, "Authorization: Bearer sk-upstream-ok"),
                   "upstream should receive upstream bearer credential") != 0 ||
            expect(contains(upstream_ok.captured_request, "POST /v1/responses HTTP/1.1"),
                   "upstream should receive native responses path") != 0 ||
            expect(contains(upstream_ok.captured_request, "\"model\":\"gpt-5.5\""),
                   "upstream should receive model body") != 0 ||
            expect(contains(upstream_ok.captured_request, "X-Revlm-Service-Tier: priority"),
                   "upstream should receive normalized service tier header") != 0) {
            std::cerr << ok << '\n' << upstream_ok.captured_request << '\n';
            return 1;
        }

        const auto rows =
            revlm::sql_query_rows(*db, "SELECT model,service_tier,input_tokens,output_tokens,cache_read_tokens,"
                                       "cache_creation_5m_tokens,channel_id,is_stream "
                                       "FROM requests WHERE request_id='2002001' LIMIT 1");
        if (expect(rows.size() == 1, "usage event should be written before response completes") != 0 ||
            expect(rows[0][0].value_or("") == "gpt-5.5", "usage event should record model") != 0 ||
            expect(rows[0][1].value_or("") == "priority", "usage should record effective service tier") != 0 ||
            expect(rows[0][2].value_or("") == "7", "usage should record input tokens") != 0 ||
            expect(rows[0][3].value_or("") == "3", "usage should record output tokens") != 0 ||
            expect(rows[0][4].value_or("") == "2", "usage should record cache read tokens") != 0 ||
            expect(rows[0][5].value_or("") == "1", "usage should record cache creation tokens") != 0 ||
            expect(rows[0][6].value_or("") == std::to_string(success_channel_id),
                   "usage should record upstream channel id") != 0 ||
            expect(rows[0][7].value_or("") == "0", "non-stream should record is_stream=0") != 0) {
            std::cerr << "usage row mismatch\n";
            return 1;
        }

        const std::string bad_body = "{\"model\":\"claude-opus-4-8\",\"input\":\"hello\"}";
        const std::string bad = api_request("/v1/responses", raw_token, bad_body, "2002003");
        if (expect(contains(bad, "HTTP/1.1 404 Not Found"), "unreachable model should be rejected before proxying") !=
            0) {
            std::cerr << bad << '\n';
            return 1;
        }

        MockUpstreamServer upstream_input_tokens;
        upstream_input_tokens.start("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                                    "{\"input_tokens\":12,\"cache_read_input_tokens\":3}");
        success_ch.base_url = "http://127.0.0.1:" + std::to_string(upstream_input_tokens.port);
        if (!channel_store.update_channel(success_ch)) {
            std::cerr << "failed to update success channel base_url\n";
            return 1;
        }
        const std::string input_tokens_body = "{\"model\":\"gpt-5.5\",\"input\":\"hello\",\"service_tier\":\"fast\"}";
        std::cerr << "[responses-test] input_tokens\n";
        const std::string input_tokens =
            api_request("/v1/responses/input_tokens", raw_token, input_tokens_body, "2002004");
        upstream_input_tokens.join();
        if (expect(contains(input_tokens, "HTTP/1.1 200 OK"), "input_tokens request should succeed") != 0 ||
            expect(contains(input_tokens, "\"input_tokens\":12"), "input_tokens response should pass through body") !=
                0 ||
            expect(contains(upstream_input_tokens.captured_request, "POST /v1/responses/input_tokens HTTP/1.1"),
                   "upstream should receive native input_tokens path") != 0 ||
            expect(contains(upstream_input_tokens.captured_request, "X-Revlm-Service-Tier: priority"),
                   "fast service tier should normalize to priority") != 0) {
            std::cerr << input_tokens << '\n' << upstream_input_tokens.captured_request << '\n';
            return 1;
        }

        revlm::sql_exec(*db, "DELETE FROM requests");
        MockUpstreamServer upstream_stream;
        upstream_stream.start("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
                              "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_stream_1\","
                              "\"model\":\"gpt-5.5\"}}\n\n"
                              "data: {\"type\":\"response.completed\",\"response\":{\"model\":\"gpt-5.5\","
                              "\"service_tier\":\"priority\",\"usage\":{\"input_tokens\":9,\"output_tokens\":4,"
                              "\"cache_read_input_tokens\":1}}}\n\n"
                              "data: [DONE]\n\n");
        success_ch.base_url = "http://127.0.0.1:" + std::to_string(upstream_stream.port);
        if (!channel_store.update_channel(success_ch)) {
            std::cerr << "failed to update success channel for stream test\n";
            return 1;
        }
        std::cerr << "[responses-test] stream\n";
        const std::string stream_body =
            "{\"model\":\"gpt-5.5\",\"input\":\"hello\",\"stream\":true,\"service_tier\":\"priority\"}";
        ::httplib::Request stream_req;
        stream_req.method = "POST";
        stream_req.path = "/v1/responses";
        stream_req.body = stream_body;
        stream_req.set_header("Authorization", "Bearer " + raw_token);
        stream_req.set_header("Content-Type", "application/json");
        int stream_pair[2]{ -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, stream_pair) != 0) {
            std::cerr << "socketpair failed: " << std::strerror(errno) << '\n';
            return 1;
        }
        revlm::Request usage;
        usage.id = 2002005;
        usage.user_id = user_id;
        usage.token_id = token_id;
        usage.channel_id = success_channel_id;
        usage.endpoint = "/v1/responses";
        usage.method = "POST";
        usage.request_id = "2002005";
        usage.is_stream = true;
        revlm::ResponsesProxyExecuteOptions options;
        options.client_fd = stream_pair[0];
        const auto stream_result = revlm::handle_responses_proxy_request(stream_req, "POST", "/v1/responses", "2002005",
                                                                         group_id, usage, options);
        ::close(stream_pair[0]);
        const std::string stream_response = recv_until_close(stream_pair[1]);
        ::close(stream_pair[1]);
        upstream_stream.join();
        if (expect(contains(stream_response, "HTTP/1.1 200 OK"), "stream responses should succeed") != 0 ||
            expect(contains(stream_response, "text/event-stream"),
                   "stream response should preserve SSE content type") != 0 ||
            expect(contains(stream_response, "\"type\":\"response.completed\""),
                   "stream response should proxy responses SSE payloads") != 0 ||
            expect(stream_result.handled_stream, "stream path should mark handled_stream") != 0 ||
            expect(stream_result.stream_status == 200, "stream path should report status 200") != 0) {
            std::cerr << stream_response << '\n';
            return 1;
        }
        const auto stream_rows =
            revlm::sql_query_rows(*db, "SELECT input_tokens,output_tokens,cache_read_tokens,is_stream,model "
                                       "FROM requests WHERE request_id='2002005' "
                                       "ORDER BY id DESC LIMIT 1");
        if (expect(stream_rows.size() == 1, "stream request should write usage event") != 0 ||
            expect(stream_rows[0][0].value_or("") == "9", "stream input tokens should be extracted") != 0 ||
            expect(stream_rows[0][1].value_or("") == "4", "stream output tokens should be extracted") != 0 ||
            expect(stream_rows[0][2].value_or("") == "1", "stream cache read tokens should be extracted") != 0 ||
            expect(stream_rows[0][3].value_or("") == "1", "stream request should record is_stream=1") != 0 ||
            expect(stream_rows[0][4].value_or("") == "gpt-5.5", "stream model should be recorded") != 0) {
            return 1;
        }

    } catch (const std::exception &err) {
        std::cerr << "responses mysql test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
