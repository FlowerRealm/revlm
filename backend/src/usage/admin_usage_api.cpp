#include "usage/admin_usage_api.hpp"

#include "auth/users.hpp"
#include "billing/billing.hpp"
#include "channels/channels.hpp"
#include "server/http_server.hpp"
#include "store/mysql.hpp"
#include "usage/usage.hpp"
#include "usage/usage_aggregation.hpp"
#include "usage/usage_queries.hpp"
#include "util/http_query.hpp"
#include "util/user_input.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

constexpr std::string_view kAdminTimeZone = "Asia/Shanghai";

struct AdminUsageRange {
    time_t since_utc = 0;
    time_t until_utc = 0;
    std::string start;
    std::string end;
    std::string since_local;
    std::string until_local;
    bool all_time = false;
};

struct AdminWindowStats {
    long long requests = 0;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_input_tokens = 0;
    long long cache_creation_input_tokens = 0;
    std::string committed_usd = "0";
    long long first_token_latency_sum = 0;
    long long first_token_latency_samples = 0;
    long long decode_tokens = 0;
    long long decode_latency_ms = 0;
};

std::mutex &tz_mutex()
{
    static std::mutex m;
    return m;
}

std::string json_escape(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    return out;
}

HttpResponse api_json_response(std::string body, std::string_view request_id, const std::vector<Header> &headers = {})
{
    return http_response(200, "OK", body, "application/json; charset=utf-8", request_id, headers);
}

std::string api_success(std::string_view data_json = {})
{
    if (data_json.empty()) {
        return "{\"success\":true,\"message\":\"\",\"data\":null}\n";
    }
    return std::string{ "{\"success\":true,\"message\":\"\",\"data\":" } + std::string{ data_json } + "}\n";
}

std::string api_failure(std::string_view message)
{
    return std::string{ "{\"success\":false,\"message\":\"" } + json_escape(message) + "\"}\n";
}

void next_date(int &year, int &month, int &day)
{
    day += 1;
    const int dim = days_in_month(year, month);
    if (day > dim) {
        day = 1;
        month += 1;
        if (month > 12) {
            month = 1;
            year += 1;
        }
    }
}

time_t timegm_utc(std::tm tm)
{
#if defined(_GNU_SOURCE) || defined(__USE_MISC) || defined(__APPLE__) || defined(__FreeBSD__)
    return ::timegm(&tm);
#else
    const char *old_tz = std::getenv("TZ");
    std::string old = old_tz == nullptr ? std::string{} : std::string{ old_tz };
    ::setenv("TZ", "UTC", 1);
    ::tzset();
    const time_t value = std::mktime(&tm);
    if (old_tz == nullptr) {
        ::unsetenv("TZ");
    } else {
        ::setenv("TZ", old.c_str(), 1);
    }
    ::tzset();
    return value;
#endif
}

class ScopedTimezone {
public:
    explicit ScopedTimezone(std::string_view time_zone)
    {
        tz_mutex().lock();
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
        tz_mutex().unlock();
    }

private:
    bool had_old_ = false;
    std::string old_;
};

time_t utc_seconds_from_local_date(int year, int month, int day, std::string_view time_zone)
{
    ScopedTimezone guard(time_zone);
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::tm local_tm_from_utc(time_t seconds, std::string_view time_zone)
{
    ScopedTimezone guard(time_zone);
    std::tm tm{};
    localtime_r(&seconds, &tm);
    return tm;
}

std::string format_local_datetime(time_t seconds, std::string_view time_zone, std::string_view pattern)
{
    const std::tm tm = local_tm_from_utc(seconds, time_zone);
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), std::string{ pattern }.c_str(), &tm);
    return buffer;
}

std::string mysql_datetime_from_unix(long long unix_seconds)
{
    std::tm tm{};
    const time_t ts = static_cast<time_t>(unix_seconds);
    gmtime_r(&ts, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return buffer;
}

std::string decimal_to_string(double value, int precision = 6)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    std::string s = out.str();
    while (s.size() > 1 && s.find('.') != std::string::npos && s.back() == '0') {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return s;
}

std::string format_percent(double ratio)
{
    return decimal_to_string(ratio * 100.0, 3);
}

long long parse_i64_or_zero(const std::optional<std::string> &value)
{
    if (!value.has_value()) {
        return 0;
    }
    long long out = 0;
    const auto [ptr, ec] = std::from_chars(value->data(), value->data() + value->size(), out);
    if (ec != std::errc{} || ptr != value->data() + value->size()) {
        return 0;
    }
    return out;
}

HttpResponse require_root(std::string_view raw_request, const Config &config, std::string_view request_id)
{
    const WebSessionAuth auth = authenticate_root_web_session(raw_request, config);
    if (auth.ok) {
        return HttpResponse{};
    }
    std::vector<Header> headers;
    if (auth.clear_cookie) {
        headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
    }
    return api_json_response(api_failure(auth.failure_message.empty() ? "未登录" : auth.failure_message), request_id,
                             headers);
}

std::optional<AdminUsageRange> resolve_admin_usage_range(MysqlConnection &conn,
                                                         const std::map<std::string, std::string> &params,
                                                         time_t now_utc, std::string &error)
{
    error.clear();
    AdminUsageRange out;
    const std::tm now_local = local_tm_from_utc(now_utc, kAdminTimeZone);
    const int today_year = now_local.tm_year + 1900;
    const int today_month = now_local.tm_mon + 1;
    const int today_day = now_local.tm_mday;
    const std::string today = format_local_datetime(now_utc, kAdminTimeZone, "%Y-%m-%d");

    const std::string all_time_raw = query_param_value(params, "all_time");
    if (!all_time_raw.empty() && !parse_bool_flag(all_time_raw, out.all_time)) {
        error = "all_time 不合法";
        return std::nullopt;
    }

    std::string start = trim_ascii(query_param_value(params, "start"));
    std::string end = trim_ascii(query_param_value(params, "end"));

    if (out.all_time) {
        const auto rows = conn.query_rows("SELECT MIN(time) FROM usage_events");
        if (!rows.empty() && rows[0].size() > 0 && rows[0][0].has_value() && !rows[0][0]->empty()) {
            std::tm first_tm{};
            std::istringstream in{ *rows[0][0] };
            in >> std::get_time(&first_tm, "%Y-%m-%d %H:%M:%S");
            const time_t first_utc = timegm_utc(first_tm);
            start = format_local_datetime(first_utc, kAdminTimeZone, "%Y-%m-%d");
            end = today;
        } else {
            start.clear();
            end.clear();
        }
    }
    if (start.empty()) {
        start = today;
    }
    if (end.empty()) {
        end = start;
    }

    int start_y = 0;
    int start_m = 0;
    int start_d = 0;
    int end_y = 0;
    int end_m = 0;
    int end_d = 0;
    if (!parse_date_yyyy_mm_dd(start, start_y, start_m, start_d)) {
        error = "start 不合法（格式：YYYY-MM-DD）";
        return std::nullopt;
    }
    if (!parse_date_yyyy_mm_dd(end, end_y, end_m, end_d)) {
        error = "end 不合法（格式：YYYY-MM-DD）";
        return std::nullopt;
    }

    out.since_utc = utc_seconds_from_local_date(start_y, start_m, start_d, kAdminTimeZone);
    const time_t end_start = utc_seconds_from_local_date(end_y, end_m, end_d, kAdminTimeZone);
    next_date(end_y, end_m, end_d);
    const time_t end_exclusive = utc_seconds_from_local_date(end_y, end_m, end_d, kAdminTimeZone);
    if (out.since_utc >= end_exclusive) {
        error = "start 不能晚于 end";
        return std::nullopt;
    }

    out.start = start;
    out.end = end;
    out.since_local = format_local_datetime(out.since_utc, kAdminTimeZone, "%Y-%m-%d %H:%M");
    const time_t today_start = utc_seconds_from_local_date(today_year, today_month, today_day, kAdminTimeZone);
    if (end_start >= today_start) {
        out.end = today;
        out.until_utc = now_utc;
        out.until_local = format_local_datetime(now_utc, kAdminTimeZone, "%Y-%m-%d %H:%M");
    } else {
        out.until_utc = end_exclusive;
        out.until_local = format_local_datetime(end_exclusive - 1, kAdminTimeZone, "%Y-%m-%d %H:%M");
    }
    return out;
}

UsageQueryFilters build_usage_query_filters(const std::map<std::string, std::string> &params,
                                            const AdminUsageRange &range, int limit, std::string &error)
{
    error.clear();
    UsageQueryFilters filters;
    filters.limit = limit;
    filters.start = mysql_datetime_from_unix(static_cast<long long>(range.since_utc));
    filters.end_exclusive = mysql_datetime_from_unix(static_cast<long long>(range.until_utc));

    const std::string user_id_raw = trim_ascii(query_param_value(params, "user_id"));
    if (!user_id_raw.empty()) {
        long long user_id = 0;
        if (!parse_i64(user_id_raw, user_id) || user_id <= 0) {
            error = "user_id 不合法";
            return filters;
        }
        filters.user_id = user_id;
    }

    const std::string channel_id_raw = trim_ascii(query_param_value(params, "channel_id"));
    if (!channel_id_raw.empty()) {
        long long channel_id = 0;
        if (!parse_i64(channel_id_raw, channel_id) || channel_id <= 0) {
            error = "channel_id 不合法";
            return filters;
        }
        filters.channel_id = channel_id;
    }

    const std::string model = trim_ascii(query_param_value(params, "model"));
    if (!model.empty()) {
        filters.model = model;
    }

    const std::string q_user = trim_ascii(query_param_value(params, "q_user"));
    if (!q_user.empty()) {
        filters.q_user = q_user;
    }
    const std::string q_channel = trim_ascii(query_param_value(params, "q_channel"));
    if (!q_channel.empty()) {
        filters.q_channel = q_channel;
    }
    const std::string q_model = trim_ascii(query_param_value(params, "q_model"));
    if (!q_model.empty()) {
        filters.q_model = q_model;
    }

    const std::string before_id_raw = trim_ascii(query_param_value(params, "before_id"));
    if (!before_id_raw.empty()) {
        long long before_id = 0;
        if (!parse_i64(before_id_raw, before_id) || before_id <= 0) {
            error = "before_id 不合法";
            return filters;
        }
        filters.before_id = before_id;
    }
    const std::string after_id_raw = trim_ascii(query_param_value(params, "after_id"));
    if (!after_id_raw.empty()) {
        long long after_id = 0;
        if (!parse_i64(after_id_raw, after_id) || after_id <= 0) {
            error = "after_id 不合法";
            return filters;
        }
        filters.after_id = after_id;
    }
    if (filters.before_id.has_value() && filters.after_id.has_value()) {
        error = "before_id 与 after_id 不能同时使用";
    }
    return filters;
}

bool has_text_search_filters(const UsageQueryFilters &filters)
{
    return (filters.q_user.has_value() && !filters.q_user->empty()) ||
           (filters.q_channel.has_value() && !filters.q_channel->empty()) ||
           (filters.q_model.has_value() && !filters.q_model->empty());
}

AdminWindowStats query_window_stats_raw(MysqlConnection &conn, const UsageQueryFilters &filters)
{
    std::vector<std::string> conditions;
    conditions.push_back("ue.time >= " + conn.quote(*filters.start));
    conditions.push_back("ue.time < " + conn.quote(*filters.end_exclusive));
    if (filters.user_id.has_value()) {
        conditions.push_back("ue.user_id=" + std::to_string(*filters.user_id));
    }
    if (filters.channel_id.has_value()) {
        conditions.push_back("ue.channel_id=" + std::to_string(*filters.channel_id));
    }
    if (filters.model.has_value()) {
        conditions.push_back("ue.model=" + conn.quote(*filters.model));
    }
    if (filters.q_user.has_value() && !filters.q_user->empty()) {
        const std::string pattern = conn.quote("%" + *filters.q_user + "%");
        conditions.push_back("(u.email LIKE " + pattern + " OR u.username LIKE " + pattern + ")");
    }
    if (filters.q_channel.has_value() && !filters.q_channel->empty()) {
        const std::string pattern = conn.quote("%" + *filters.q_channel + "%");
        conditions.push_back("EXISTS (SELECT 1 FROM channels uc WHERE uc.id=ue.channel_id AND uc.name LIKE " + pattern +
                             ")");
    }
    if (filters.q_model.has_value() && !filters.q_model->empty()) {
        conditions.push_back("ue.model LIKE " + conn.quote("%" + *filters.q_model + "%"));
    }

    std::string where = " WHERE ";
    for (size_t i = 0; i < conditions.size(); ++i) {
        if (i > 0) {
            where += " AND ";
        }
        where += conditions[i];
    }

    const auto rows = conn.query_rows(
        "SELECT COALESCE(SUM(CASE WHEN ue.state='committed' THEN 1 ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' THEN COALESCE(ue.input_tokens,0) ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' THEN COALESCE(ue.output_tokens,0) ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' THEN COALESCE(ue.cache_read_input_tokens,0) ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' THEN COALESCE(ue.cache_creation_input_tokens,0) ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' THEN ue.committed_usd ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' AND ue.first_token_latency_ms>0 THEN ue.first_token_latency_ms ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' AND ue.first_token_latency_ms>0 THEN 1 ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' AND COALESCE(ue.output_tokens,0)>0 AND ue.latency_ms>ue.first_token_latency_ms "
        "THEN ue.output_tokens ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' AND COALESCE(ue.output_tokens,0)>0 AND ue.latency_ms>ue.first_token_latency_ms "
        "THEN ue.latency_ms-ue.first_token_latency_ms ELSE 0 END),0) "
        "FROM usage_events ue JOIN users u ON u.id=ue.user_id" +
        where);

    AdminWindowStats stats;
    if (rows.empty() || rows[0].size() < 10) {
        return stats;
    }
    const MysqlResultRow &row = rows[0];
    stats.requests = parse_i64_or_zero(row[0]);
    stats.input_tokens = parse_i64_or_zero(row[1]);
    stats.output_tokens = parse_i64_or_zero(row[2]);
    stats.cache_read_input_tokens = parse_i64_or_zero(row[3]);
    stats.cache_creation_input_tokens = parse_i64_or_zero(row[4]);
    stats.committed_usd = row[5].value_or("0");
    stats.first_token_latency_sum = parse_i64_or_zero(row[6]);
    stats.first_token_latency_samples = parse_i64_or_zero(row[7]);
    stats.decode_tokens = parse_i64_or_zero(row[8]);
    stats.decode_latency_ms = parse_i64_or_zero(row[9]);
    return stats;
}

AdminWindowStats query_window_stats(MysqlConnection &conn, const UsageQueryFilters &filters,
                                    const AdminUsageRange &range)
{
    if (range.all_time || has_text_search_filters(filters) || !filters.start.has_value() ||
        !filters.end_exclusive.has_value()) {
        return query_window_stats_raw(conn, filters);
    }

    UsageAggregationStore store(conn);
    UsagePrimaryFilter primary;
    if (filters.user_id.has_value()) {
        primary.user_id = *filters.user_id;
    }
    if (filters.channel_id.has_value()) {
        primary.channel_id = *filters.channel_id;
    }
    const UsageTotals totals = store.sum_primary(*filters.start, *filters.end_exclusive, primary);
    AdminWindowStats stats;
    stats.requests = totals.requests;
    stats.input_tokens = totals.input_tokens;
    stats.output_tokens = totals.output_tokens;
    stats.cache_read_input_tokens = totals.cache_read_input_tokens;
    stats.cache_creation_input_tokens = totals.cache_creation_input_tokens;
    stats.committed_usd = billing_format_usd_from_micros(totals.committed_usd_micros);
    stats.first_token_latency_sum = totals.first_token_latency_sum;
    stats.first_token_latency_samples = totals.first_token_samples;
    stats.decode_tokens = totals.output_tokens_for_tps;
    stats.decode_latency_ms = totals.decode_latency_sum;
    return stats;
}

AdminWindowStats query_recent_window_stats(MysqlConnection &conn, time_t now_utc)
{
    UsageQueryFilters filters;
    filters.start = mysql_datetime_from_unix(static_cast<long long>(now_utc - 60));
    filters.end_exclusive = mysql_datetime_from_unix(static_cast<long long>(now_utc + 1));
    return query_window_stats_raw(conn, filters);
}

std::string top_users_json(MysqlConnection &conn, const UsageQueryFilters &filters)
{
    std::vector<std::string> conditions;
    conditions.push_back("ue.time >= " + conn.quote(*filters.start));
    conditions.push_back("ue.time < " + conn.quote(*filters.end_exclusive));
    conditions.push_back("ue.state='committed'");
    if (filters.user_id.has_value()) {
        conditions.push_back("ue.user_id=" + std::to_string(*filters.user_id));
    }
    if (filters.channel_id.has_value()) {
        conditions.push_back("ue.channel_id=" + std::to_string(*filters.channel_id));
    }
    if (filters.model.has_value()) {
        conditions.push_back("ue.model=" + conn.quote(*filters.model));
    }
    if (filters.q_user.has_value() && !filters.q_user->empty()) {
        const std::string pattern = conn.quote("%" + *filters.q_user + "%");
        conditions.push_back("(u.email LIKE " + pattern + " OR u.username LIKE " + pattern + ")");
    }
    if (filters.q_channel.has_value() && !filters.q_channel->empty()) {
        const std::string pattern = conn.quote("%" + *filters.q_channel + "%");
        conditions.push_back("EXISTS (SELECT 1 FROM channels uc WHERE uc.id=ue.channel_id AND uc.name LIKE " + pattern +
                             ")");
    }
    if (filters.q_model.has_value() && !filters.q_model->empty()) {
        conditions.push_back("ue.model LIKE " + conn.quote("%" + *filters.q_model + "%"));
    }

    std::string where = " WHERE ";
    for (size_t i = 0; i < conditions.size(); ++i) {
        if (i > 0) {
            where += " AND ";
        }
        where += conditions[i];
    }

    const auto rows = conn.query_rows(
        "SELECT ue.user_id,u.email,u.role,u.status,COALESCE(SUM(ue.committed_usd),0) "
        "FROM usage_events ue JOIN users u ON u.id=ue.user_id" +
        where +
        " GROUP BY ue.user_id,u.email,u.role,u.status ORDER BY SUM(ue.committed_usd) DESC, ue.user_id DESC LIMIT 50");

    std::string out = "[";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        const MysqlResultRow &row = rows[i];
        out += "{\"user_id\":" + std::to_string(parse_i64_or_zero(row[0])) + ",\"email\":\"" +
               json_escape(row[1].value_or("")) + "\",\"role\":\"" + json_escape(row[2].value_or("")) +
               "\",\"status\":" + std::to_string(parse_i64_or_zero(row[3])) + ",\"committed_usd\":\"" +
               json_escape(row[4].value_or("0")) + "\"}";
    }
    out += "]";
    return out;
}

std::string window_summary_json(const AdminUsageRange &range, const AdminWindowStats &stats,
                                const AdminWindowStats &recent_stats)
{
    const double total_tokens = static_cast<double>(stats.input_tokens + stats.output_tokens);
    const double cached_tokens = static_cast<double>(stats.cache_read_input_tokens + stats.cache_creation_input_tokens);
    const double avg_latency = stats.first_token_latency_samples > 0 ?
                                   static_cast<double>(stats.first_token_latency_sum) /
                                       static_cast<double>(stats.first_token_latency_samples) :
                                   0.0;
    const double tps = stats.decode_latency_ms > 0 ? static_cast<double>(stats.decode_tokens) * 1000.0 /
                                                         static_cast<double>(stats.decode_latency_ms) :
                                                     0.0;
    return std::string{ "{\"window\":\"统计区间\",\"since\":\"" } + json_escape(range.since_local) + "\",\"until\":\"" +
           json_escape(range.until_local) + "\",\"requests\":" + std::to_string(stats.requests) +
           ",\"tokens\":" + std::to_string(stats.input_tokens + stats.output_tokens) +
           ",\"input_tokens\":" + std::to_string(stats.input_tokens) +
           ",\"output_tokens\":" + std::to_string(stats.output_tokens) +
           ",\"cached_tokens\":" + std::to_string(stats.cache_read_input_tokens + stats.cache_creation_input_tokens) +
           ",\"cache_ratio\":\"" + format_percent(total_tokens > 0 ? cached_tokens / total_tokens : 0.0) +
           "\",\"rpm\":\"" + decimal_to_string(static_cast<double>(recent_stats.requests), 3) + "\",\"tpm\":\"" +
           decimal_to_string(static_cast<double>(recent_stats.input_tokens + recent_stats.output_tokens), 3) +
           "\",\"avg_first_token_latency\":\"" + decimal_to_string(avg_latency, 3) + "\",\"tokens_per_second\":\"" +
           decimal_to_string(tps, 3) + "\",\"committed_usd\":\"" + json_escape(stats.committed_usd) + "\"}";
}

std::vector<UsageTimeSeriesPoint> query_admin_time_series(MysqlConnection &conn, const UsageQueryFilters &filters,
                                                          std::string_view granularity)
{
    const char *bucket_expr = granularity == "day" ? "%Y-%m-%d 00:00" : "%Y-%m-%d %H:00";
    std::vector<std::string> conditions;
    conditions.push_back("ue.time >= " + conn.quote(*filters.start));
    conditions.push_back("ue.time < " + conn.quote(*filters.end_exclusive));
    if (filters.user_id.has_value()) {
        conditions.push_back("ue.user_id=" + std::to_string(*filters.user_id));
    }
    if (filters.channel_id.has_value()) {
        conditions.push_back("ue.channel_id=" + std::to_string(*filters.channel_id));
    }
    if (filters.model.has_value()) {
        conditions.push_back("ue.model=" + conn.quote(*filters.model));
    }
    if (filters.q_user.has_value() && !filters.q_user->empty()) {
        const std::string pattern = conn.quote("%" + *filters.q_user + "%");
        conditions.push_back("(u.email LIKE " + pattern + " OR u.username LIKE " + pattern + ")");
    }
    if (filters.q_channel.has_value() && !filters.q_channel->empty()) {
        const std::string pattern = conn.quote("%" + *filters.q_channel + "%");
        conditions.push_back("EXISTS (SELECT 1 FROM channels uc WHERE uc.id=ue.channel_id AND uc.name LIKE " + pattern +
                             ")");
    }
    if (filters.q_model.has_value() && !filters.q_model->empty()) {
        conditions.push_back("ue.model LIKE " + conn.quote("%" + *filters.q_model + "%"));
    }

    std::string where = " WHERE ";
    for (size_t i = 0; i < conditions.size(); ++i) {
        if (i > 0) {
            where += " AND ";
        }
        where += conditions[i];
    }

    const auto rows = conn.query_rows(
        "SELECT DATE_FORMAT(CONVERT_TZ(ue.time,'UTC'," + conn.quote(std::string{ kAdminTimeZone }) + ")," +
        conn.quote(bucket_expr) +
        "),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' THEN 1 ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' THEN COALESCE(ue.input_tokens,0)+COALESCE(ue.output_tokens,0) ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' THEN ue.committed_usd ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' THEN COALESCE(ue.cache_read_input_tokens,0)+COALESCE(ue.cache_creation_input_tokens,0) ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' THEN COALESCE(ue.input_tokens,0)+COALESCE(ue.output_tokens,0) ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' AND ue.first_token_latency_ms>0 THEN ue.first_token_latency_ms ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' AND ue.first_token_latency_ms>0 THEN 1 ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' AND COALESCE(ue.output_tokens,0)>0 AND ue.latency_ms>ue.first_token_latency_ms THEN ue.output_tokens ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN ue.state='committed' AND COALESCE(ue.output_tokens,0)>0 AND ue.latency_ms>ue.first_token_latency_ms THEN ue.latency_ms-ue.first_token_latency_ms ELSE 0 END),0) "
        "FROM usage_events ue JOIN users u ON u.id=ue.user_id" +
        where + " GROUP BY 1 ORDER BY 1 ASC");

    std::vector<UsageTimeSeriesPoint> out;
    out.reserve(rows.size());
    for (const MysqlResultRow &row : rows) {
        if (row.size() < 10 || !row[0].has_value()) {
            continue;
        }
        UsageTimeSeriesPoint point;
        point.bucket = row[0].value_or("");
        point.requests = parse_i64_or_zero(row[1]);
        point.tokens = parse_i64_or_zero(row[2]);
        point.committed_usd = row[3].has_value() ? std::stod(*row[3]) : 0.0;
        const double cached_tokens = row[4].has_value() ? std::stod(*row[4]) : 0.0;
        const double total_tokens = row[5].has_value() ? std::stod(*row[5]) : 0.0;
        point.cache_ratio = total_tokens > 0 ? (cached_tokens / total_tokens) * 100.0 : 0.0;
        const double latency_sum = row[6].has_value() ? std::stod(*row[6]) : 0.0;
        const double latency_samples = row[7].has_value() ? std::stod(*row[7]) : 0.0;
        point.avg_first_token_latency = latency_samples > 0 ? latency_sum / latency_samples : 0.0;
        const double decode_tokens = row[8].has_value() ? std::stod(*row[8]) : 0.0;
        const double decode_ms = row[9].has_value() ? std::stod(*row[9]) : 0.0;
        point.tokens_per_second = decode_ms > 0 ? decode_tokens * 1000.0 / decode_ms : 0.0;
        out.push_back(std::move(point));
    }
    return out;
}

std::string time_series_json(const AdminUsageRange &range, std::string_view granularity,
                             const std::vector<UsageTimeSeriesPoint> &points)
{
    std::string out = "{\"admin_time_zone\":\"" + json_escape(kAdminTimeZone) + "\",\"start\":\"" +
                      json_escape(range.start) + "\",\"end\":\"" + json_escape(range.end) + "\",\"granularity\":\"" +
                      json_escape(granularity) + "\",\"points\":[";
    for (size_t i = 0; i < points.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        const UsageTimeSeriesPoint &point = points[i];
        out += "{\"bucket\":\"" + json_escape(point.bucket) + "\",\"requests\":" + std::to_string(point.requests) +
               ",\"tokens\":" + std::to_string(point.tokens) +
               ",\"committed_usd\":" + decimal_to_string(point.committed_usd) +
               ",\"cache_ratio\":" + decimal_to_string(point.cache_ratio) +
               ",\"avg_first_token_latency\":" + decimal_to_string(point.avg_first_token_latency) +
               ",\"tokens_per_second\":" + decimal_to_string(point.tokens_per_second) + "}";
    }
    out += "]}";
    return out;
}

std::pair<std::string, std::string> default_time_series_range(time_t now_utc, std::string_view granularity,
                                                              std::string start, std::string end, bool all_time)
{
    if (all_time || !start.empty() || !end.empty()) {
        return { start, end };
    }
    if (granularity == "day") {
        const time_t earlier = now_utc - 29 * 24 * 3600;
        return { format_local_datetime(earlier, kAdminTimeZone, "%Y-%m-%d"),
                 format_local_datetime(now_utc, kAdminTimeZone, "%Y-%m-%d") };
    }
    return { format_local_datetime(now_utc, kAdminTimeZone, "%Y-%m-%d"),
             format_local_datetime(now_utc, kAdminTimeZone, "%Y-%m-%d") };
}

} // namespace

HttpResponse admin_dashboard_http_response(std::string_view raw_request, const Config &config,
                                           std::string_view request_id)
{
    if (HttpResponse auth = require_root(raw_request, config, request_id); !auth.body.empty()) {
        return auth;
    }
    try {
        MysqlConnection conn(config.db_dsn);
        UserStore users(conn);
        ChannelStore channels(conn);

        const time_t now_utc = std::time(nullptr);
        const std::tm now_local = local_tm_from_utc(now_utc, kAdminTimeZone);
        const time_t today_start = utc_seconds_from_local_date(now_local.tm_year + 1900, now_local.tm_mon + 1,
                                                               now_local.tm_mday, kAdminTimeZone);

        UsageQueryFilters filters;
        filters.start = mysql_datetime_from_unix(static_cast<long long>(today_start));
        filters.end_exclusive = mysql_datetime_from_unix(static_cast<long long>(now_utc));

        AdminUsageRange range;
        range.since_utc = today_start;
        range.until_utc = now_utc;
        range.start = format_local_datetime(today_start, kAdminTimeZone, "%Y-%m-%d");
        range.end = range.start;

        const AdminWindowStats stats = query_window_stats(conn, filters, range);

        const std::string data =
            std::string{ "{\"admin_time_zone\":\"" } + json_escape(kAdminTimeZone) +
            "\",\"stats\":{\"users_count\":" + std::to_string(users.count_users()) +
            ",\"channels_count\":" + std::to_string(static_cast<long long>(channels.list_channels().size())) +
            ",\"endpoints_count\":" + std::to_string(static_cast<long long>(channels.list_channels().size())) +
            ",\"requests_today\":" + std::to_string(stats.requests) +
            ",\"tokens_today\":" + std::to_string(stats.input_tokens + stats.output_tokens) +
            ",\"input_tokens_today\":" + std::to_string(stats.input_tokens) +
            ",\"output_tokens_today\":" + std::to_string(stats.output_tokens) + ",\"cost_today\":\"" +
            json_escape(stats.committed_usd) + "\"}}";
        return api_json_response(api_success(data), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("读取统计失败"), request_id);
    }
}

HttpResponse admin_usage_page_http_response(std::string_view raw_request, const Config &config,
                                            std::string_view request_id, std::string_view target)
{
    if (HttpResponse auth = require_root(raw_request, config, request_id); !auth.body.empty()) {
        return auth;
    }

    const std::map<std::string, std::string> params = parse_query_map(target);
    int limit = 50;
    const std::string limit_raw = query_param_value(params, "limit");
    if (!limit_raw.empty()) {
        if (!parse_i32(limit_raw, limit)) {
            return api_json_response(api_failure("limit 不合法"), request_id);
        }
    }
    if (limit < 10) {
        limit = 10;
    }
    if (limit > 200) {
        limit = 200;
    }

    bool include_summary = true;
    const std::string summary_raw = query_param_value(params, "summary");
    if (!summary_raw.empty() && !parse_bool_flag(summary_raw, include_summary)) {
        return api_json_response(api_failure("summary 不合法"), request_id);
    }

    try {
        MysqlConnection conn(config.db_dsn);
        const time_t now_utc = std::time(nullptr);
        std::string range_error;
        const auto range = resolve_admin_usage_range(conn, params, now_utc, range_error);
        if (!range.has_value()) {
            return api_json_response(api_failure(range_error), request_id);
        }

        std::string filter_error;
        UsageQueryFilters filters = build_usage_query_filters(params, *range, limit, filter_error);
        if (!filter_error.empty()) {
            return api_json_response(api_failure(filter_error), request_id);
        }

        UsageQueryStore query_store(conn);
        const UsageEventsPage page = query_store.list_admin_usage_events(filters);

        std::string data = std::string{ "{\"admin_time_zone\":\"" } + json_escape(kAdminTimeZone) + "\",\"now\":\"" +
                           format_local_datetime(now_utc, kAdminTimeZone, "%Y-%m-%d %H:%M") + "\",\"start\":\"" +
                           json_escape(range->start) + "\",\"end\":\"" + json_escape(range->end) +
                           "\",\"limit\":" + std::to_string(limit);

        if (include_summary) {
            const AdminWindowStats stats = query_window_stats(conn, filters, *range);
            const AdminWindowStats recent_stats = query_recent_window_stats(conn, now_utc);
            data += ",\"window\":" + window_summary_json(*range, stats, recent_stats);
            data += ",\"top_users\":" + top_users_json(conn, filters);
        }

        data += "," + usage_events_page_to_admin_json(page);
        data += "}";
        return api_json_response(api_success(data), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse admin_usage_users_suggest_http_response(std::string_view raw_request, const Config &config,
                                                     std::string_view request_id, std::string_view target)
{
    if (HttpResponse auth = require_root(raw_request, config, request_id); !auth.body.empty()) {
        return auth;
    }
    const std::map<std::string, std::string> params = parse_query_map(target);
    const std::string q = trim_ascii(query_param_value(params, "q"));
    if (q.empty()) {
        return api_json_response(api_success("[]"), request_id);
    }
    int limit = 20;
    const std::string limit_raw = query_param_value(params, "limit");
    if (!limit_raw.empty() && !parse_i32(limit_raw, limit)) {
        return api_json_response(api_failure("limit 不合法"), request_id);
    }
    if (limit <= 0) {
        limit = 20;
    }
    if (limit > 50) {
        limit = 50;
    }
    try {
        MysqlConnection conn(config.db_dsn);
        UsageStore store(conn);
        const auto users = store.suggest_users(q, limit);
        std::string data = "[";
        for (size_t i = 0; i < users.size(); ++i) {
            if (i > 0) {
                data += ",";
            }
            data += "{\"id\":" + std::to_string(users[i].id) + ",\"email\":\"" + json_escape(users[i].email) +
                    "\",\"username\":\"" + json_escape(users[i].username) + "\"}";
        }
        data += "]";
        return api_json_response(api_success(data), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询失败"), request_id);
    }
}

HttpResponse admin_usage_channels_suggest_http_response(std::string_view raw_request, const Config &config,
                                                        std::string_view request_id, std::string_view target)
{
    if (HttpResponse auth = require_root(raw_request, config, request_id); !auth.body.empty()) {
        return auth;
    }
    const std::map<std::string, std::string> params = parse_query_map(target);
    const std::string q = trim_ascii(query_param_value(params, "q"));
    if (q.empty()) {
        return api_json_response(api_success("[]"), request_id);
    }
    int limit = 20;
    const std::string limit_raw = query_param_value(params, "limit");
    if (!limit_raw.empty() && !parse_i32(limit_raw, limit)) {
        return api_json_response(api_failure("limit 不合法"), request_id);
    }
    if (limit <= 0) {
        limit = 20;
    }
    if (limit > 50) {
        limit = 50;
    }
    try {
        MysqlConnection conn(config.db_dsn);
        std::string range_error;
        const auto range = resolve_admin_usage_range(conn, params, std::time(nullptr), range_error);
        if (!range.has_value()) {
            return api_json_response(api_failure(range_error), request_id);
        }
        UsageStore store(conn);
        const auto channels =
            store.suggest_usage_channels(mysql_datetime_from_unix(static_cast<long long>(range->since_utc)),
                                         mysql_datetime_from_unix(static_cast<long long>(range->until_utc)), q, limit);
        std::string data = "[";
        for (size_t i = 0; i < channels.size(); ++i) {
            if (i > 0) {
                data += ",";
            }
            data += "{\"id\":" + std::to_string(channels[i].id) + ",\"name\":\"" + json_escape(channels[i].name) +
                    "\",\"type\":\"" + json_escape(channels[i].type) + "\"}";
        }
        data += "]";
        return api_json_response(api_success(data), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询失败"), request_id);
    }
}

HttpResponse admin_usage_models_suggest_http_response(std::string_view raw_request, const Config &config,
                                                      std::string_view request_id, std::string_view target)
{
    if (HttpResponse auth = require_root(raw_request, config, request_id); !auth.body.empty()) {
        return auth;
    }
    const std::map<std::string, std::string> params = parse_query_map(target);
    const std::string q = trim_ascii(query_param_value(params, "q"));
    if (q.empty()) {
        return api_json_response(api_success("[]"), request_id);
    }
    int limit = 20;
    const std::string limit_raw = query_param_value(params, "limit");
    if (!limit_raw.empty() && !parse_i32(limit_raw, limit)) {
        return api_json_response(api_failure("limit 不合法"), request_id);
    }
    if (limit <= 0) {
        limit = 20;
    }
    if (limit > 50) {
        limit = 50;
    }
    try {
        MysqlConnection conn(config.db_dsn);
        std::string range_error;
        const auto range = resolve_admin_usage_range(conn, params, std::time(nullptr), range_error);
        if (!range.has_value()) {
            return api_json_response(api_failure(range_error), request_id);
        }
        UsageStore store(conn);
        const auto models =
            store.suggest_usage_models(mysql_datetime_from_unix(static_cast<long long>(range->since_utc)),
                                       mysql_datetime_from_unix(static_cast<long long>(range->until_utc)), q, limit);
        std::string data = "[";
        for (size_t i = 0; i < models.size(); ++i) {
            if (i > 0) {
                data += ",";
            }
            data += "{\"model\":\"" + json_escape(models[i]) + "\"}";
        }
        data += "]";
        return api_json_response(api_success(data), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询失败"), request_id);
    }
}

HttpResponse admin_usage_event_detail_http_response(std::string_view raw_request, const Config &config,
                                                    std::string_view request_id, long long event_id)
{
    if (HttpResponse auth = require_root(raw_request, config, request_id); !auth.body.empty()) {
        return auth;
    }
    if (event_id <= 0) {
        return api_json_response(api_failure("event_id 不合法"), request_id);
    }
    try {
        MysqlConnection conn(config.db_dsn);
        UsageQueryStore store(conn);
        const auto detail = store.get_admin_usage_event_detail(event_id);
        if (!detail.has_value()) {
            return api_json_response(api_failure("not found"), request_id);
        }
        return api_json_response(api_success(usage_event_detail_to_json(*detail)), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询失败"), request_id);
    }
}

HttpResponse admin_usage_timeseries_http_response(std::string_view raw_request, const Config &config,
                                                  std::string_view request_id, std::string_view target)
{
    if (HttpResponse auth = require_root(raw_request, config, request_id); !auth.body.empty()) {
        return auth;
    }

    std::map<std::string, std::string> params = parse_query_map(target);
    std::string granularity = lowercase_ascii(trim_ascii(query_param_value(params, "granularity")));
    if (granularity.empty()) {
        granularity = "hour";
    }
    if (granularity != "hour" && granularity != "day") {
        return api_json_response(api_failure("granularity 仅支持 hour/day"), request_id);
    }

    bool all_time = false;
    const std::string all_time_raw = query_param_value(params, "all_time");
    if (!all_time_raw.empty() && !parse_bool_flag(all_time_raw, all_time)) {
        return api_json_response(api_failure("all_time 不合法"), request_id);
    }

    const time_t now_utc = std::time(nullptr);
    auto [default_start, default_end] =
        default_time_series_range(now_utc, granularity, trim_ascii(query_param_value(params, "start")),
                                  trim_ascii(query_param_value(params, "end")), all_time);
    if (query_param_value(params, "start").empty()) {
        params["start"] = default_start;
    }
    if (query_param_value(params, "end").empty()) {
        params["end"] = default_end;
    }

    try {
        MysqlConnection conn(config.db_dsn);
        std::string range_error;
        const auto range = resolve_admin_usage_range(conn, params, now_utc, range_error);
        if (!range.has_value()) {
            return api_json_response(api_failure(range_error), request_id);
        }

        std::string filter_error;
        UsageQueryFilters filters = build_usage_query_filters(params, *range, 50, filter_error);
        if (!filter_error.empty()) {
            return api_json_response(api_failure(filter_error), request_id);
        }

        const auto points = query_admin_time_series(conn, filters, granularity);
        return api_json_response(api_success(time_series_json(*range, granularity, points)), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询失败"), request_id);
    }
}

} // namespace revlm
