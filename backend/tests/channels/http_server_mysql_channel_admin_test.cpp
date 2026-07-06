#include "auth/users.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "server/http_server.hpp"
#include "store/migrations.hpp"
#include "store/mysql.hpp"

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
                                 long long user_id, std::string_view session_value, const revlm::Config &config,
                                 const revlm::BuildInfo &build, std::string_view request_id)
{
    std::string req = std::string(method) + " " + std::string(target) +
                      " HTTP/1.1\r\nHost: test\r\nRevlm-User: " + std::to_string(user_id) +
                      "\r\nCookie: revlm_session=" + std::string(session_value) + "\r\n";
    if (!body.empty()) {
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    return revlm::handle_http_request(req, config, build, false, request_id);
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
        (void)revlm::apply_migrations(dsn, "internal/store/migrations", "", 30);

        revlm::MysqlConnection conn(dsn);
        conn.exec("DELETE FROM usage_events");
        conn.exec("DELETE FROM channel_group_members");
        conn.exec("DELETE FROM channel_groups");
        conn.exec("DELETE FROM channels");
        conn.exec("DELETE FROM session_bindings");
        conn.exec("DELETE FROM user_tokens");
        conn.exec("DELETE FROM users");

        revlm::UserStore user_store(conn);
        const long long root_id = user_store.create_user(
            revlm::CreateUserInput{ "root@example.com", "root", revlm::hash_password("password"), "root" });
        const auto root = user_store.get_user_by_email("root@example.com");
        if (!root.has_value()) {
            std::cerr << "failed to create root user\n";
            return 1;
        }
        if (root->id != root_id) {
            std::cerr << "created root user id mismatch\n";
            return 1;
        }

        const revlm::SessionCookie root_session = revlm::make_session_cookie(root_id, "tmp-session-secret");
        user_store.upsert_session_binding_payload(root_id, revlm::session_binding_hash(root_session.key), "web",
                                                  mysql_datetime_from_unix(root_session.expires_unix));

        revlm::ChannelStore channel_store(conn);
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
        group_store.reload(conn);
        const long long group_id = group_store.create_channel_group("tmp-a006-group", "", 1.0);
        if (!group_store.add_channel_group_member(group_id, ch)) {
            std::cerr << "failed to bind channel group member\n";
            return 1;
        }

        conn.exec("INSERT INTO usage_events("
                  "time,request_id,user_id,token_id,state,model,requested_service_tier,service_tier,"
                  "service_tier_downgrade_reason,input_tokens,output_tokens,committed_usd,created_at,updated_at,"
                  "channel_id,cache_read_input_tokens,cache_creation_input_tokens,first_token_latency_ms,latency_ms"
                  ") VALUES("
                  "'2026-06-24 10:00:00','tmp-a006-req-1'," +
                  std::to_string(root->id) +
                  ",1,'committed','gpt-4.1',NULL,NULL,NULL,120,80,1.500000,"
                  "'2026-06-24 10:00:00','2026-06-24 10:00:00'," +
                  std::to_string(channel_id) + ",50,30,250,1250)");
        conn.exec("INSERT INTO usage_events("
                  "time,request_id,user_id,token_id,state,model,requested_service_tier,service_tier,"
                  "service_tier_downgrade_reason,input_tokens,output_tokens,committed_usd,created_at,updated_at,"
                  "channel_id,cache_read_input_tokens,cache_creation_input_tokens,first_token_latency_ms,latency_ms"
                  ") VALUES("
                  "'2026-06-24 11:00:00','tmp-a006-req-2'," +
                  std::to_string(root->id) +
                  ",1,'committed','gpt-4.1',NULL,NULL,NULL,60,40,0.500000,"
                  "'2026-06-24 11:00:00','2026-06-24 11:00:00'," +
                  std::to_string(channel_id) + ",20,10,150,650)");

        revlm::Config config;
        config.db_dsn = dsn;
        config.session_secret = "tmp-session-secret";
        revlm::BuildInfo build{ "test-version", "test-date" };

        const std::string page =
            request_with_session("GET", "/api/channel/page?start=2026-06-24%2000:00:00&end=2026-06-24%2023:59:59", "",
                                 root_id, root_session.value, config, build, "req-page");
        if (expect(contains(page, "\"success\":true"), "channel page should succeed") != 0 ||
            expect(contains(page, "\"requests\":2"), "overview should aggregate request count") != 0 ||
            expect(contains(page, "\"tokens\":300"), "overview should aggregate total tokens") != 0 ||
            expect(contains(page, "\"committed_usd\":\"2\""), "overview committed usd should be real") != 0 ||
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
            "", root_id, root_session.value, config, build, "req-series");
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
