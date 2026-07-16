#include "auth/session.hpp"
#include "store/mysql_test_env.hpp"
#include "users/users.hpp"
#include "util/user_input.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "server/http_server.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <optional>
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

std::string mysql_datetime_from_unix(long long unix_seconds)
{
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return buffer;
}

std::string request_with_session(std::string_view method, std::string_view target, std::string_view body,
                                 long long user_id, std::string_view session_value, std::string_view request_id)
{
    std::string req = std::string(method) + " " + std::string(target) +
                      " HTTP/1.1\r\nHost: test\r\nRevlm-User: " + std::to_string(user_id) +
                      "\r\nCookie: revlm_session=" + std::string(session_value) + "\r\n";
    if (!body.empty()) {
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    return revlm::handle_http_request(req, false, request_id);
}

bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

} // namespace

int main()
{
    const char *dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping channel admin MySQL test\n";
        return 0;
    }

    try {
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);
        {
            revlm::Config __runtime_cfg;
            __runtime_cfg.db_dsn = dsn;
            __runtime_cfg.session_secret = "tmp-session-secret";
            revlm::test::install_test_runtime(__runtime_cfg);
        }

        revlm::sql_exec(*db, "DELETE FROM requests");
        revlm::sql_exec(*db, "DELETE FROM channel_group_members");
        revlm::sql_exec(*db, "DELETE FROM channel_groups");
        revlm::sql_exec(*db, "DELETE FROM channels");
        revlm::sql_exec(*db, "DELETE FROM session_bindings");
        revlm::sql_exec(*db, "DELETE FROM user_tokens");
        revlm::sql_exec(*db, "DELETE FROM users");

        revlm::UserStore &user_store = revlm::UserStore::instance();
        revlm::SessionStore &sessions = revlm::SessionStore::instance();
        revlm::User root_id_user = revlm::User("root@example.com", "root", revlm::hash_password("password"), "root");
        root_id_user.status = 1;
        const long long root_id = user_store.create_user(std::move(root_id_user));
        const revlm::User root = user_store.get_user_by_email("root@example.com");
        if (root.id == 0) {
            std::cerr << "failed to create root user\n";
            return 1;
        }
        if (root.id != root_id) {
            std::cerr << "created root user id mismatch\n";
            return 1;
        }

        const revlm::SessionCookie root_session = revlm::make_session_cookie(root_id, "tmp-session-secret");
        sessions.upsert_session_binding_payload(root_id, revlm::session_binding_hash(root_session.key), "web",
                                                mysql_datetime_from_unix(root_session.expires_unix));

        revlm::ChannelStore &channel_store = revlm::ChannelStore::instance();
        revlm::Channel ch;
        ch.type = 2;
        ch.name = "OpenAI A006";
        ch.priority = 7;
        ch.status = true;
        ch.base_url = "https://api.openai.com/v1";
        ch.api_key = "sk-test-a006";
        if (!channel_store.create_channel(ch)) {
            std::cerr << "failed to create channel\n";
            return 1;
        }
        const long long channel_id = ch.id;

        revlm::ChannelGroupStore &group_store = revlm::ChannelGroupStore::instance();
        const long long group_id = group_store.create_channel_group("tmp-a006-group", "", 1.0);
        if (!group_store.add_channel_group_member(group_id, ch)) {
            std::cerr << "failed to bind channel group member\n";
            return 1;
        }

        // gpt-5.5: input $5/1M, output $30/1M, cache_read $0.5/1M →
        // (120*5 + 80*30 + 50*0.5)/1e6 + (60*5 + 40*30 + 20*0.5)/1e6 = 0.004535
        revlm::sql_exec(*db, "INSERT INTO requests("
                             "id,time,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                             "user_id,token_id,channel_id,model,"
                             "input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
                             "output_tokens,tier_multiplier,channel_multiplier,is_stream"
                             ") VALUES("
                             "6001,'2026-06-24 10:00:00','/v1/responses','POST',200,1250,250," +
                                 std::to_string(root.id) + ",1," + std::to_string(channel_id) +
                                 ",'gpt-5.5',120,50,30,0,80,1.0,1.0,0)");
        revlm::sql_exec(*db, "INSERT INTO requests("
                             "id,time,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                             "user_id,token_id,channel_id,model,"
                             "input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
                             "output_tokens,tier_multiplier,channel_multiplier,is_stream"
                             ") VALUES("
                             "6002,'2026-06-24 11:00:00','/v1/responses','POST',200,650,150," +
                                 std::to_string(root.id) + ",1," + std::to_string(channel_id) +
                                 ",'gpt-5.5',60,20,10,0,40,1.0,1.0,0)");

        const std::string page =
            request_with_session("GET", "/api/channel/page?start=2026-06-24%2000:00:00&end=2026-06-24%2023:59:59", "",
                                 root_id, root_session.value, "req-page");
        if (expect(contains(page, "\"success\":true"), "channel page should succeed") != 0 ||
            expect(contains(page, "\"requests\":2"), "overview should aggregate request count") != 0 ||
            expect(contains(page, "\"tokens\":300"), "overview should aggregate total tokens") != 0 ||
            expect(contains(page, "\"usd\":\"0.004535\""), "overview used usd should use solve_price") != 0 ||
            expect(contains(page, "\"cache_ratio\":\"36.7\""), "page should compute cache ratio") != 0 ||
            expect(contains(page, "\"avg_first_token_latency\":\"200\""),
                   "page should compute avg first token latency") != 0 ||
            expect(contains(page, "\"tokens_per_second\":\"80\""), "page should compute tokens per second") != 0 ||
            expect(contains(page, "\"in_use\":true"), "page should expose in_use from group membership") != 0) {
            std::cerr << page << '\n';
            return 1;
        }

        const std::string series = request_with_session(
            "GET",
            "/api/channel/" + std::to_string(channel_id) +
                "/timeseries?start=2026-06-24%2000:00:00&end=2026-06-24%2023:59:59&granularity=hour",
            "", root_id, root_session.value, "req-series");
        if (expect(contains(series, "\"success\":true"), "timeseries should succeed") != 0 ||
            expect(contains(series, "\"bucket\":\"2026-06-24 10:00:00\""), "timeseries should contain first bucket") !=
                0 ||
            expect(contains(series, "\"cache_ratio\":40.0"), "timeseries should compute cache ratio") != 0 ||
            expect(contains(series, "\"avg_first_token_latency\":250.0"),
                   "timeseries should compute first-token latency") != 0 ||
            expect(contains(series, "\"tokens_per_second\":80.00"), "timeseries should compute tokens per second") !=
                0) {
            std::cerr << series << '\n';
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "channel admin MySQL test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
