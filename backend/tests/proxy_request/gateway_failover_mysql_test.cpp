#include "auth/users.hpp"
#include "util/user_input.hpp"
#include "channels/channel_groups.hpp"
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

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <openssl/sha.h>
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

        step("seed");
        revlm::sql_exec(*db, "DELETE FROM requests");
        revlm::sql_exec(*db, "DELETE FROM channel_group_members");
        revlm::sql_exec(*db, "DELETE FROM token_channel_groups");
        revlm::sql_exec(*db, "DELETE FROM channel_groups");
        revlm::sql_exec(*db, "DELETE FROM channels");
        revlm::sql_exec(*db, "DELETE FROM user_tokens");
        revlm::sql_exec(*db, "DELETE FROM session_bindings");
        revlm::sql_exec(*db, "DELETE FROM users");

        revlm::UserStore user_store(*db);
        revlm::TokenStore &token_store = user_store.tokens();
        long long user_id = 0;
        long long token_id = 0;
        std::string raw_token;
        bool found_first_start = false;
        for (int i = 0; i < 16; ++i) {
            const std::string suffix = std::to_string(i);
            const char letter = static_cast<char>('a' + (i % 26));
            revlm::User user("g008" + suffix + "@example.com", std::string("chat") + letter,
                             revlm::hash_password("password"), "user");
            user.status = 1;
            user_id = user_store.create_user(std::move(user));
            raw_token = "sk_tmp_g008_failover_" + suffix;
            token_id = token_store.create_user_token(user_id, odb::nullable<std::string>{}, raw_token);
            const std::string route_key = std::to_string(user_id) + ":" + std::to_string(token_id) + ":gpt-5.5";
            if (sequential_route_start_index(sha256_hex(route_key), 2) == 0) {
                found_first_start = true;
                break;
            }
        }
        if (!found_first_start) {
            std::cerr << "failed to find deterministic first-channel route key\n";
            return 1;
        }
        revlm::User funded = user_store.get_user_by_id(user_id);
        funded.balance_usd = 10.0;
        if (!user_store.update_user(funded)) {
            std::cerr << "failed to fund failover user\n";
            return 1;
        }

        revlm::ChannelGroupStore group_store(*db);
        const long long group_id = group_store.create_channel_group("tmp_g008_openai", "", 1.0);
        if (!token_store.replace_token_channel_groups(token_id, { "tmp_g008_openai" })) {
            std::cerr << "bind token groups failed\n";
            return 1;
        }

        MockUpstreamServer failing_upstream;
        failing_upstream.start(
            "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
            "{\"error\":{\"message\":\"temporary\",\"type\":\"server_error\"}}");
        MockUpstreamServer healthy_upstream;
        healthy_upstream.start(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
            "{\"id\":\"chatcmpl-failover\",\"object\":\"chat.completion\",\"model\":\"gpt-5.5\","
            "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"recovered\"}}],"
            "\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3,\"total_tokens\":10}}");

        revlm::ChannelStore channel_store(*db);

        revlm::Channel primary_ch;
        primary_ch.type = 2;
        primary_ch.name = "tmp-g008-primary";
        primary_ch.priority = 10;
        primary_ch.status = true;
        primary_ch.base_url = "http://127.0.0.1:" + std::to_string(failing_upstream.port);
        primary_ch.api_key = "upstream-secret-1";
        if (!channel_store.create_channel(primary_ch)) {
            std::cerr << "create primary channel failed\n";
            return 1;
        }
        const long long primary_channel_id = primary_ch.id;
        if (!group_store.add_channel_group_member(group_id, primary_ch)) {
            std::cerr << "bind primary channel group member failed\n";
            return 1;
        }
        revlm::Channel secondary_ch;
        secondary_ch.type = 2;
        secondary_ch.name = "tmp-g008-secondary";
        secondary_ch.priority = 5;
        secondary_ch.status = true;
        secondary_ch.base_url = "http://127.0.0.1:" + std::to_string(healthy_upstream.port);
        secondary_ch.api_key = "upstream-secret-2";
        if (!channel_store.create_channel(secondary_ch)) {
            std::cerr << "create secondary channel failed\n";
            return 1;
        }
        const long long secondary_channel_id = secondary_ch.id;
        if (!group_store.add_channel_group_member(group_id, secondary_ch)) {
            std::cerr << "bind secondary channel group member failed\n";
            return 1;
        }
        revlm::Config config;
        config.addr = "127.0.0.1:18081";
        config.db_dsn = dsn;
        config.session_secret = "tmp-session-secret";
        config.gateway_max_retry_attempts = 2;
        config.gateway_max_failover_switches = 1;
        config.gateway_max_retry_elapsed_ms = 1000;

        const std::string body = "{\"model\":\"gpt-5.5\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
        const std::string request =
            "POST /v1/chat/completions HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer " + raw_token +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

        step("failover-request");
        const std::string failover_response = revlm::handle_http_request(request, config, false, "2008001");
        step("failover-join");
        failing_upstream.join();
        healthy_upstream.join();
        step("failover-assert");

        if (expect(contains(failover_response, "HTTP/1.1 200 OK"),
                   "failover request should recover on second channel") != 0 ||
            expect(!failing_upstream.captured_request.empty(), "failover should try the first channel") != 0 ||
            expect(!healthy_upstream.captured_request.empty(), "failover should rotate to the second channel") != 0 ||
            expect(contains(failing_upstream.captured_request, "Authorization: Bearer upstream-secret-1"),
                   "first attempt should use primary channel key") != 0 ||
            expect(contains(healthy_upstream.captured_request, "Authorization: Bearer upstream-secret-2"),
                   "second attempt should use rotated channel key") != 0) {
            std::cerr << failover_response << '\n';
            return 1;
        }

        const auto usage_rows = revlm::sql_query_rows(*db, "SELECT status_code,input_tokens,output_tokens,channel_id "
                                                           "FROM requests WHERE request_id='2008001' "
                                                           "ORDER BY id DESC LIMIT 1");
        if (expect(!usage_rows.empty(), "failover request should write usage event") != 0 ||
            expect(usage_rows[0][0].value_or("") == "200", "usage should record final success status") != 0 ||
            expect(usage_rows[0][1].value_or("") == "7", "usage should record winning prompt tokens") != 0 ||
            expect(usage_rows[0][2].value_or("") == "3", "usage should record winning completion tokens") != 0 ||
            expect(usage_rows[0][3].value_or("") == std::to_string(secondary_channel_id),
                   "usage should point at winning channel") != 0) {
            return 1;
        }

        step("parse-request");
        revlm::sql_exec(*db, "DELETE FROM requests");
        primary_ch.base_url = "://bad-upstream";
        if (!channel_store.update_channel(primary_ch)) {
            std::cerr << "failed to update primary channel base_url\n";
            return 1;
        }
        config.gateway_max_retry_attempts = 1;
        config.gateway_max_failover_switches = 0;
        const std::string parse_failure_response = revlm::handle_http_request(request, config, false, "2008002");
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
            expect(parse_usage_rows[0][2].value_or("") == std::to_string(primary_channel_id),
                   "parse failure usage should keep attempted channel id") != 0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "gateway failover MySQL test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
