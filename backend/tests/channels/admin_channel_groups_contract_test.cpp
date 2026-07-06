#include "auth/users.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "server/http_server.hpp"
#include "store/migrations.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

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

int expect_contains(const std::string &haystack, const char *needle, const char *message)
{
    if (haystack.find(needle) != std::string::npos) {
        return 0;
    }
    std::cerr << message << '\n' << haystack << '\n';
    return 1;
}

void reset_contract_tables(revlm::MysqlConnection &conn)
{
    conn.exec("DELETE FROM channel_group_members");
    conn.exec("DELETE FROM channels");
    conn.exec("DELETE FROM token_channel_groups");
    conn.exec("DELETE FROM channel_groups");
    conn.exec("DELETE FROM users");
}

std::string json_request(std::string_view method, std::string_view path, long long user_id,
                         std::string_view session_cookie, std::string_view body)
{
    std::ostringstream out;
    out << method << ' ' << path << " HTTP/1.1\r\n"
        << "Host: test\r\n"
        << "Revlm-User: " << user_id << "\r\n"
        << "Cookie: revlm_session=" << session_cookie << "\r\n";
    if (!body.empty()) {
        out << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n";
    }
    out << "\r\n";
    out << body;
    return out.str();
}

revlm::Channel make_channel(std::string name, int priority, std::string base_url = "")
{
    revlm::Channel channel;
    channel.type = 2;
    channel.name = std::move(name);
    channel.status = true;
    channel.priority = priority;
    channel.base_url = std::move(base_url);
    return channel;
}

} // namespace

int main()
{
    const char *dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping admin channel group contract test\n";
        return 0;
    }

    try {
        (void)revlm::apply_migrations(dsn, std::string{ REVLM_SOURCE_DIR } + "/internal/store/migrations", "", 30);

        revlm::MysqlConnection conn(dsn);
        reset_contract_tables(conn);

        revlm::Config config;
        config.role = revlm::RuntimeRole::Api;
        config.db_dsn = dsn;
        config.session_secret = "tmp-admin-channel-groups-contract-secret";
        const revlm::BuildInfo build{ "test-version", "test-date" };

        revlm::UserStore users(conn);
        const long long user_id = users.create_user({
            .email = "root@example.com",
            .username = "rootadmin",
            .password_hash = revlm::hash_password("password123"),
            .role = "root",
        });
        const revlm::SessionCookie session =
            revlm::make_session_cookie(user_id, revlm::session_secret_for_config(config));
        users.upsert_session_binding_payload(user_id, revlm::session_binding_hash(session.key), "web",
                                             "2099-01-01 00:00:00");

        revlm::ChannelGroupStore &group_store = revlm::ChannelGroupStore::instance();
        group_store.reload(conn);
        revlm::ChannelStore channel_store(conn);

        const long long group_id = group_store.create_channel_group("primary", "primary group", 1.0);

        revlm::Channel seed = make_channel("seed-channel", 11);
        revlm::Channel moved = make_channel("moved-channel", 7);
        revlm::Channel added = make_channel("added-channel", 42);
        if (!channel_store.create_channel(seed) || !channel_store.create_channel(moved) ||
            !channel_store.create_channel(added)) {
            std::cerr << "create channels failed\n";
            return 1;
        }

        group_store.create_channel_group_member(group_id, std::vector<revlm::Channel>{ seed, moved });

        const std::string add_member_body = "{\"channel_id\":" + std::to_string(added.id) + "}";
        const std::string add_member_response = revlm::handle_http_request(
            json_request("POST", "/api/admin/channel-groups/" + std::to_string(group_id) + "/children/channels",
                         user_id, session.value, add_member_body),
            config, build, false, "req-add-member");
        if (expect_contains(add_member_response, "HTTP/1.1 200 OK", "member add should return 200") != 0 ||
            expect_contains(add_member_response, "\"success\":true", "member add should succeed") != 0) {
            return 1;
        }

        const revlm::ChannelGroup group = group_store.get_channel_group_by_id(group_id);
        if (expect(group.id == group_id, "group should load") != 0 ||
            expect(group.channels.size() == 3U, "group should have three channels in order") != 0 ||
            expect(group.channels[0].id == seed.id, "first member order should match seed") != 0 ||
            expect(group.channels[1].id == moved.id, "second member order should match moved") != 0 ||
            expect(group.channels[2].id == added.id, "third member should be added channel") != 0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "admin channel group contract test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
