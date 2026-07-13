#include "server/http_server.hpp"
#include "store/migrations.hpp"
#include "store/mysql.hpp"
#include "server/tokens.hpp"
#include "auth/users.hpp"
#include "util/user_input.hpp"
#include "store/mysql_test_env.hpp"

#include <chrono>
#include <ctime>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

namespace
{

int fail(const std::string &message)
{
    std::cerr << message << '\n';
    return 1;
}

std::string must_cookie(const std::string &response)
{
    const std::string needle = "Set-Cookie: revlm_session=";
    const std::size_t start = response.find(needle);
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t value_start = start + needle.size();
    const std::size_t value_end = response.find(';', value_start);
    if (value_end == std::string::npos) {
        return {};
    }
    return response.substr(value_start, value_end - value_start);
}

bool expect_contains(const std::string &body, const std::string &needle)
{
    return body.find(needle) != std::string::npos;
}

bool expect_not_contains(const std::string &body, const std::string &needle)
{
    return body.find(needle) == std::string::npos;
}

class ScopedTimezone {
public:
    explicit ScopedTimezone(std::string_view time_zone)
    {
        mutex().lock();
        const char *old_tz = std::getenv("TZ");
        had_old_ = old_tz != nullptr;
        if (had_old_) {
            old_ = old_tz;
        }
        ::setenv("TZ", std::string{ time_zone }.c_str(), 1);
        ::tzset();
    }

    ~ScopedTimezone()
    {
        if (had_old_) {
            ::setenv("TZ", old_.c_str(), 1);
        } else {
            ::unsetenv("TZ");
        }
        ::tzset();
        mutex().unlock();
    }

private:
    static std::mutex &mutex()
    {
        static std::mutex value;
        return value;
    }

    bool had_old_ = false;
    std::string old_;
};

std::string mysql_datetime_from_unix(long long unix_seconds)
{
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return buffer;
}

std::string iso8601_from_unix(long long unix_seconds)
{
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

time_t utc_seconds_from_local_date(int year, int month, int day, std::string_view time_zone)
{
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    ScopedTimezone scoped(time_zone);
    return std::mktime(&tm);
}

std::tm local_tm_from_utc(time_t utc, std::string_view time_zone)
{
    ScopedTimezone scoped(time_zone);
    std::tm tm{};
    localtime_r(&utc, &tm);
    return tm;
}

} // namespace

int main()
{
    std::optional<revlm::test::MysqlTestEnv> env = revlm::test::prepare_mysql_test_env("a004-tz");
    if (!env.has_value()) {
        return 0;
    }

    revlm::Config config;
    config.db_dsn = env->dsn;
    config.session_secret = "tmp-a004-session-secret";
    revlm::BuildInfo build{ "test-version", "test-date" };

    const time_t now = std::time(nullptr);
    const std::tm shanghai_now = local_tm_from_utc(now, "Asia/Shanghai");
    const int year = shanghai_now.tm_year + 1900;
    const int month = shanghai_now.tm_mon + 1;
    const int day = shanghai_now.tm_mday;
    const time_t today_start_utc = utc_seconds_from_local_date(year, month, day, "Asia/Shanghai");
    const time_t in_today = today_start_utc + 8 * 3600 + 1800;
    const time_t next_local_day = today_start_utc + 32 * 3600;
    const std::string today_since = iso8601_from_unix(today_start_utc);
    const std::string today_until = iso8601_from_unix(today_start_utc + 86400 - 1);

    try {
        config.migrations_dir = "internal/store/migrations";
        config.db_migration_lock_name = "tmp-a004-tz-mysql-test";
        config.db_migration_lock_timeout_seconds = 5;
        (void)revlm::apply_migrations(config);
        revlm::MysqlConnection conn(config.db_dsn);
        conn.exec("DELETE FROM session_bindings");
        conn.exec("DELETE FROM requests");
        conn.exec("DELETE FROM user_balances");
        conn.exec("DELETE FROM user_tokens");
        conn.exec("DELETE FROM users");
        const std::string password_hash = revlm::hash_password("password");
        const std::string token_hash = revlm::token_hash("tok");
        conn.exec("INSERT INTO users(id,email,password_hash,role,status,username) VALUES"
                  "(1001,'tz@example.com'," +
                  conn.quote(password_hash) + ",'user',1,'tzuser')");
        conn.exec("INSERT INTO user_balances(user_id,usd,created_at,updated_at) VALUES"
                  "(1001,'50.00','2026-06-20 00:00:00','2026-06-20 00:00:00')");
        conn.exec("INSERT INTO user_tokens(id,user_id,name,token_hash,token_plain,status) VALUES"
                  "(2001,1001,'primary'," +
                  conn.quote(token_hash) + ",'tok',1)");
        conn.exec(
            "INSERT INTO requests("
            "id,user_id,token_id,`time`,status,model,input_tokens,output_tokens,cache_read_tokens,"
            "cache_creation_5m_tokens,cache_creation_1h_tokens,latency_ms,first_token_latency_ms,endpoint,method,"
            "status_code,is_stream,channel_id,tier_multiplier,channel_multiplier"
            ") VALUES "
            "(3001,1001,2001," +
            conn.quote(mysql_datetime_from_unix(in_today)) +
            ",'committed','gpt-5.5',100,20,0,0,0,1000,100,'/v1/chat/completions','POST',200,0,0,1.0,1.0),"
            "(3002,1001,2001," +
            conn.quote(mysql_datetime_from_unix(next_local_day)) +
            ",'committed','gpt-5.5',200,30,0,0,0,2000,200,'/v1/chat/completions','POST',200,0,0,1.0,1.0),"
            "(3003,1001,2001,'2026-06-24 00:30:00','committed','gpt-5.5',100,20,0,0,0,1000,100,"
            "'/v1/chat/completions','POST',200,0,0,1.0,1.0),"
            "(3004,1001,2001,'2026-06-24 16:30:00','committed','gpt-5.5',200,30,0,0,0,2000,200,"
            "'/v1/chat/completions','POST',200,0,0,1.0,1.0)");
    } catch (const std::exception &err) {
        return fail(std::string{ "seed failed: " } + err.what());
    }

    const std::string login = revlm::handle_http_request(
        "POST /api/user/login HTTP/1.1\r\nHost: test\r\nContent-Type: application/json\r\nContent-Length: 48\r\n\r\n"
        "{\"email\":\"tz@example.com\",\"password\":\"password\"}",
        config, build, false, "req-login");
    const std::string cookie = must_cookie(login);
    if (cookie.empty()) {
        return fail("login did not return session cookie");
    }

    const auto authed_get = [&](const std::string &target, const std::string &request_id) {
        return revlm::handle_http_request("GET " + target + " HTTP/1.1\r\nHost: test\r\nCookie: revlm_session=" +
                                              cookie + "\r\nRevlm-User: 1001\r\n\r\n",
                                          config, build, false, request_id);
    };

    const std::string dashboard = authed_get("/api/dashboard?tz=Asia/Shanghai", "req-dashboard-shanghai");
    if (!expect_contains(dashboard, "\"today_requests\":1") || !expect_contains(dashboard, "\"today_tokens\":120")) {
        return fail("dashboard Asia/Shanghai today window did not isolate local day");
    }
    if (!expect_contains(dashboard, "\"today_since\":\"" + today_since + "\"") ||
        !expect_contains(dashboard, "\"today_until\":\"" + today_until + "\"")) {
        return fail("dashboard Asia/Shanghai did not return the local-day chart window");
    }
    if (!expect_contains(dashboard, "\"cache_ratio\":0.000000") ||
        !expect_contains(dashboard, "\"avg_first_token_latency\":100.000000") ||
        !expect_contains(dashboard, "\"tokens_per_second\":20.000000")) {
        return fail("dashboard Asia/Shanghai chart payload did not match usage time-series fields");
    }
    if (expect_contains(dashboard, "\"label\":")) {
        return fail("dashboard chart payload still exposes label instead of bucket");
    }

    const std::string invalid_dashboard = authed_get("/api/dashboard?tz=Not/AZone", "req-dashboard-invalid-tz");
    if (!expect_contains(invalid_dashboard, "\"success\":false") ||
        !expect_contains(invalid_dashboard, "\"message\":\"tz \xE6\x97\xA0\xE6\x95\x88\"")) {
        return fail("dashboard accepted an invalid IANA timezone");
    }

    const std::string invalid_timeseries =
        authed_get("/api/request/timeseries?tz=Not/AZone&granularity=day", "req-timeseries-invalid-tz");
    if (!expect_contains(invalid_timeseries, "\"success\":false") ||
        !expect_contains(invalid_timeseries, "\"message\":\"tz \xE6\x97\xA0\xE6\x95\x88\"")) {
        return fail("usage timeseries accepted an invalid IANA timezone");
    }

    const std::string windows =
        authed_get("/api/request/windows?tz=America/Los_Angeles&start=2026-06-23&end=2026-06-23", "req-windows-la");
    if (!expect_contains(windows, "\"requests\":1") ||
        !expect_contains(windows, "\"since\":\"2026-06-23T07:00:00Z\"") ||
        !expect_contains(windows, "\"until\":\"2026-06-24T06:59:59Z\"")) {
        return fail("usage windows America/Los_Angeles did not use tz boundaries");
    }

    const std::string events =
        authed_get("/api/request/events?tz=America/Los_Angeles&start=2026-06-23&end=2026-06-23", "req-events-la");
    if (!expect_contains(events, "\"id\":3003") || !expect_not_contains(events, "\"id\":3004")) {
        return fail("usage events America/Los_Angeles missed same-local-day events");
    }

    const std::string timeseries_day =
        authed_get("/api/request/timeseries?tz=America/Los_Angeles&start=2026-06-23&end=2026-06-23&granularity=day",
                   "req-timeseries-day-la");
    if (!expect_contains(timeseries_day, "\"bucket\":\"2026-06-23\"") ||
        !expect_contains(timeseries_day, "\"requests\":1")) {
        return fail("usage timeseries day America/Los_Angeles did not group by local date");
    }

    const std::string timeseries_hour =
        authed_get("/api/request/timeseries?tz=Asia/Shanghai&start=2026-06-24&end=2026-06-25&granularity=hour",
                   "req-timeseries-hour-shanghai");
    if (!expect_contains(timeseries_hour, "\"bucket\":\"2026-06-24T08:00:00\"") ||
        !expect_contains(timeseries_hour, "\"bucket\":\"2026-06-25T00:00:00\"")) {
        return fail("usage timeseries hour Asia/Shanghai did not group by local hour");
    }

    return 0;
}
