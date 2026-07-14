#include "auth/session.hpp"
#include "auth/users.hpp"
#include "store/database.hpp"
#include "util/user_input.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "server/http_server.hpp"

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

void reset_contract_tables(odb::database &db)
{
    revlm::sql_exec(db, "DELETE FROM channel_group_members");
    revlm::sql_exec(db, "DELETE FROM channels");
    revlm::sql_exec(db, "DELETE FROM token_channel_groups");
    revlm::sql_exec(db, "DELETE FROM channel_groups");
    revlm::sql_exec(db, "DELETE FROM users");
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
    channel.status = 1;
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
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);

        reset_contract_tables(*db);

        revlm::Config config;
        config.db_dsn = dsn;
        config.session_secret = "tmp-admin-channel-groups-contract-secret";
        const revlm::BuildInfo build{ "test-version", "test-date" };

        revlm::UserStore users(*db);
        revlm::SessionStore sessions(*db);
        revlm::User user_id_user = revlm::User("root@example.com", "rootadmin", revlm::hash_password("password123"), "root");
        user_id_user.status = 1;
        const long long user_id = users.create_user(std::move(user_id_user));
        const revlm::SessionCookie session =
            revlm::make_session_cookie(user_id, revlm::session_secret_for_config(config));
        sessions.upsert_session_binding_payload(user_id, revlm::session_binding_hash(session.key), "web",
                                              "2099-01-01 00:00:00");

        revlm::ChannelGroupStore group_store(*db);
        revlm::ChannelStore channel_store(*db);

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
