#include "usage/usage_aggregation.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "util/strings.hpp"
#include "util/user_input.hpp"

namespace revlm
{
namespace
{

struct DateTimeParts {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

struct CoverageWindow {
    std::string start_utc;
    std::string end_utc;
};

struct MarkerRefresh {
    std::string marker;
    bool marker_exists = false;
    std::optional<long long> source_watermark;
};

constexpr std::string_view committed_usage_state = "committed";
constexpr std::string_view coverage_live_scope_user = "user";
constexpr std::string_view coverage_live_scope_token = "token";
constexpr std::string_view coverage_live_scope_channel = "channel";

int parse_fixed_int(std::string_view raw, std::string_view /*field*/)
{
    if (raw.empty()) {
        assert(false && "internal: fixed int field is empty");
        return 0;
    }
    int value = 0;
    const auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (ec != std::errc{} || ptr != raw.data() + raw.size()) {
        assert(false && "internal: fixed int field is invalid");
        return 0;
    }
    return value;
}

DateTimeParts parse_datetime(std::string_view raw)
{
    const std::string value = trim_ascii(raw);
    if (value.size() != 19 || value[4] != '-' || value[7] != '-' || value[10] != ' ' || value[13] != ':' ||
        value[16] != ':') {
        assert(false && "internal: datetime must use YYYY-MM-DD HH:MM:SS");
        return {};
    }
    DateTimeParts out;
    out.year = parse_fixed_int(std::string_view{ value }.substr(0, 4), "year");
    out.month = parse_fixed_int(std::string_view{ value }.substr(5, 2), "month");
    out.day = parse_fixed_int(std::string_view{ value }.substr(8, 2), "day");
    out.hour = parse_fixed_int(std::string_view{ value }.substr(11, 2), "hour");
    out.minute = parse_fixed_int(std::string_view{ value }.substr(14, 2), "minute");
    out.second = parse_fixed_int(std::string_view{ value }.substr(17, 2), "second");
    if (out.month < 1 || out.month > 12 || out.day < 1 || out.day > days_in_month(out.year, out.month) ||
        out.hour < 0 || out.hour > 23 || out.minute < 0 || out.minute > 59 || out.second < 0 || out.second > 59) {
        assert(false && "internal: datetime has out-of-range fields");
        return {};
    }
    return out;
}

std::string format_datetime(const DateTimeParts &dt)
{
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                  dt.second);
    return std::string{ buf };
}

std::string format_date(const DateTimeParts &dt)
{
    char buf[11];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt.year, dt.month, dt.day);
    return std::string{ buf };
}

DateTimeParts floor_to_minute(DateTimeParts dt)
{
    dt.second = 0;
    return dt;
}

DateTimeParts floor_to_hour(DateTimeParts dt)
{
    dt.minute = 0;
    dt.second = 0;
    return dt;
}

DateTimeParts floor_to_day(DateTimeParts dt)
{
    dt.hour = 0;
    dt.minute = 0;
    dt.second = 0;
    return dt;
}

DateTimeParts add_days(DateTimeParts dt, int delta)
{
    if (delta < 0) {
        assert(false && "internal: negative day delta is unsupported");
        return dt;
    }
    for (int i = 0; i < delta; ++i) {
        ++dt.day;
        if (dt.day > days_in_month(dt.year, dt.month)) {
            dt.day = 1;
            ++dt.month;
            if (dt.month > 12) {
                dt.month = 1;
                ++dt.year;
            }
        }
    }
    return dt;
}

DateTimeParts add_minutes(DateTimeParts dt, int delta_minutes)
{
    if (delta_minutes < 0) {
        assert(false && "internal: negative minute delta is unsupported");
        return dt;
    }
    int total_minutes = dt.hour * 60 + dt.minute + delta_minutes;
    dt.hour = 0;
    dt.minute = 0;
    dt.second = 0;
    const int day_delta = total_minutes / (24 * 60);
    const int remainder = total_minutes % (24 * 60);
    dt = add_days(dt, day_delta);
    dt.hour = remainder / 60;
    dt.minute = remainder % 60;
    return dt;
}

DateTimeParts add_hours(DateTimeParts dt, int delta_hours)
{
    return add_minutes(dt, delta_hours * 60);
}

long long parse_i64_default(const std::optional<std::string> &raw)
{
    if (!raw.has_value() || raw->empty()) {
        return 0;
    }
    return std::stoll(*raw);
}

void add_usage_totals_from_row(UsageTotals &totals, const MysqlResultRow &row)
{
    // Column order: requests, input, cache_read, cache_creation, output,
    // first_token_samples, first_token_latency_sum, output_tokens_for_tps, decode_latency_sum
    // (committed_usd removed from stats; totals.committed_usd_micros stays 0)
    for (size_t i = 0; i < row.size(); ++i) {
        const long long value = parse_i64_default(row[i]);
        switch (i) {
        case 0:
            totals.requests += value;
            break;
        case 1:
            totals.input_tokens += value;
            break;
        case 2:
            totals.cache_read_tokens += value;
            break;
        case 3:
            totals.cache_creation_tokens += value;
            break;
        case 4:
            totals.output_tokens += value;
            break;
        case 5:
            totals.first_token_samples += value;
            break;
        case 6:
            totals.first_token_latency_sum += value;
            break;
        case 7:
            totals.output_tokens_for_tps += value;
            break;
        case 8:
            totals.decode_latency_sum += value;
            break;
        default:
            break;
        }
    }
}

std::string sql_primary_filter(const UsagePrimaryFilter &filter, bool stats_table = false)
{
    std::string where;
    if (filter.user_id.has_value()) {
        where += " AND user_id=" + std::to_string(*filter.user_id);
    }
    if (filter.token_id.has_value()) {
        where += " AND token_id=" + std::to_string(*filter.token_id);
    }
    if (filter.channel_id.has_value()) {
        const std::string channel_col = stats_table ? "upstream_channel_id" : "channel_id";
        if (*filter.channel_id == 0) {
            where += " AND (" + channel_col + "=0 OR " + channel_col + " IS NULL)";
        } else {
            where += " AND " + channel_col + "=" + std::to_string(*filter.channel_id);
        }
    }
    return where;
}

std::string stat_column_for_grain(UsageRangeGrain grain)
{
    switch (grain) {
    case UsageRangeGrain::day:
        return "stat_date";
    case UsageRangeGrain::hour:
        return "stat_hour";
    case UsageRangeGrain::minute:
        return "stat_minute";
    case UsageRangeGrain::raw:
        return "time";
    }
    assert(false && "internal: unknown grain");
    return {};
}

std::string stats_table_for_grain(UsageRangeGrain grain)
{
    switch (grain) {
    case UsageRangeGrain::day:
        return "usage_daily_stats";
    case UsageRangeGrain::hour:
        return "usage_hourly_stats";
    case UsageRangeGrain::minute:
        return "usage_minute_stats";
    case UsageRangeGrain::raw:
        return "usage_events";
    }
    assert(false && "internal: unknown grain");
    return {};
}

std::string scope_stats_table_for_grain(UsageRangeGrain grain)
{
    switch (grain) {
    case UsageRangeGrain::day:
        return "usage_daily_scope_stats";
    case UsageRangeGrain::hour:
        return "usage_hourly_scope_stats";
    case UsageRangeGrain::minute:
        return "usage_minute_scope_stats";
    case UsageRangeGrain::raw:
        return "usage_events";
    }
    assert(false && "internal: unknown grain");
    return {};
}

std::string model_stats_table_for_grain(UsageRangeGrain grain)
{
    switch (grain) {
    case UsageRangeGrain::day:
        return "usage_daily_model_stats";
    case UsageRangeGrain::hour:
        return "usage_hourly_model_stats";
    case UsageRangeGrain::minute:
        return "usage_minute_model_stats";
    case UsageRangeGrain::raw:
        return "usage_events";
    }
    assert(false && "internal: unknown grain");
    return {};
}

std::string marker_table_for_grain(UsageRangeGrain grain)
{
    switch (grain) {
    case UsageRangeGrain::day:
        return "usage_daily_stats_backfilled";
    case UsageRangeGrain::hour:
        return "usage_hourly_stats_backfilled";
    case UsageRangeGrain::minute:
        return "usage_minute_stats_backfilled";
    case UsageRangeGrain::raw:
        return {};
    }
    assert(false && "internal: unknown grain");
    return {};
}

CoverageWindow to_coverage_window(UsageRangeGrain grain, std::string_view start_utc, std::string_view end_utc)
{
    const DateTimeParts start = parse_datetime(start_utc);
    const DateTimeParts end = parse_datetime(end_utc);
    if (format_datetime(start) >= format_datetime(end)) {
        return {};
    }
    CoverageWindow out;
    if (grain == UsageRangeGrain::day) {
        out.start_utc = format_datetime(floor_to_day(start));
        out.end_utc = format_datetime(floor_to_day(end));
    } else if (grain == UsageRangeGrain::hour) {
        out.start_utc = format_datetime(floor_to_hour(start));
        out.end_utc = format_datetime(floor_to_hour(end));
    } else if (grain == UsageRangeGrain::minute) {
        out.start_utc = format_datetime(floor_to_minute(start));
        out.end_utc = format_datetime(floor_to_minute(end));
    } else {
        out.start_utc = format_datetime(start);
        out.end_utc = format_datetime(end);
    }
    if (out.start_utc >= out.end_utc) {
        out.start_utc.clear();
        out.end_utc.clear();
    }
    return out;
}

std::optional<std::string> normalize_live_scope_type(std::string_view scope_type)
{
    const std::string normalized = trim_ascii(scope_type);
    if (normalized == coverage_live_scope_user || normalized == coverage_live_scope_token ||
        normalized == coverage_live_scope_channel) {
        return normalized;
    }
    return std::nullopt;
}

bool cache_contains(const std::vector<UsageAggregationStore::CoverageCacheEntry> &cache, std::string_view key,
                    std::uint64_t epoch, const std::optional<long long> &source_watermark)
{
    for (const auto &entry : cache) {
        if (entry.epoch == epoch && entry.key == key && entry.source_watermark == source_watermark) {
            return true;
        }
    }
    return false;
}

void cache_insert(std::vector<UsageAggregationStore::CoverageCacheEntry> &cache, std::string key, std::uint64_t epoch,
                  std::optional<long long> source_watermark)
{
    if (cache.size() >= 512) {
        cache.erase(cache.begin(), cache.begin() + static_cast<long>(cache.size() / 2));
    }
    cache.push_back({ .key = std::move(key), .epoch = epoch, .source_watermark = std::move(source_watermark) });
}

std::string coverage_cache_key(UsageRangeGrain grain, std::string_view start_utc, std::string_view end_utc)
{
    return std::string{ usage_range_grain_name(grain) } + "|" + std::string{ start_utc } + "|" + std::string{ end_utc };
}

std::vector<std::string> enumerate_markers(UsageRangeGrain grain, std::string_view start_utc, std::string_view end_utc)
{
    std::vector<std::string> out;
    CoverageWindow window = to_coverage_window(grain, start_utc, end_utc);
    if (window.start_utc.empty()) {
        return out;
    }
    DateTimeParts cursor = parse_datetime(window.start_utc);
    const DateTimeParts end = parse_datetime(window.end_utc);
    while (format_datetime(cursor) < format_datetime(end)) {
        if (grain == UsageRangeGrain::day) {
            out.push_back(format_date(cursor));
            cursor = add_days(cursor, 1);
        } else if (grain == UsageRangeGrain::hour) {
            out.push_back(format_datetime(cursor));
            cursor = add_hours(cursor, 1);
        } else if (grain == UsageRangeGrain::minute) {
            out.push_back(format_datetime(cursor));
            cursor = add_minutes(cursor, 1);
        } else {
            break;
        }
    }
    return out;
}

std::string sql_value_list(MysqlConnection &conn, const std::vector<std::string> &values)
{
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += conn.quote(values[i]);
    }
    return out;
}

std::vector<MarkerRefresh> markers_to_refresh(MysqlConnection &conn, UsageRangeGrain grain, std::string_view start_utc,
                                              std::string_view end_utc)
{
    const std::vector<std::string> expected = enumerate_markers(grain, start_utc, end_utc);
    if (expected.empty()) {
        return {};
    }
    const std::string column = stat_column_for_grain(grain);
    const std::string marker_table = marker_table_for_grain(grain);
    const auto marker_rows = conn.query_rows("SELECT " + column + " FROM " + marker_table + " WHERE " + column +
                                             " IN (" + sql_value_list(conn, expected) + ")");
    std::unordered_set<std::string> existing_markers;
    existing_markers.reserve(marker_rows.size());
    for (const MysqlResultRow &row : marker_rows) {
        if (!row.empty() && row[0].has_value()) {
            existing_markers.insert(*row[0]);
        }
    }

    const std::string source_bucket = grain == UsageRangeGrain::day  ? "DATE(`time`)" :
                                      grain == UsageRangeGrain::hour ? "DATE_FORMAT(`time`,'%Y-%m-%d %H:00:00')" :
                                                                       "DATE_FORMAT(`time`,'%Y-%m-%d %H:%i:00')";
    const auto source_rows = conn.query_rows(
        "SELECT " + source_bucket + ", MAX(id) FROM usage_events WHERE status=" + sql_quote(committed_usage_state) +
        " AND time>=" + conn.quote(start_utc) + " AND time<" + conn.quote(end_utc) + " GROUP BY 1");
    std::unordered_map<std::string, std::optional<long long>> source_watermark_by_marker;
    source_watermark_by_marker.reserve(source_rows.size());
    for (const MysqlResultRow &row : source_rows) {
        if (row.size() >= 2 && row[0].has_value()) {
            source_watermark_by_marker[*row[0]] =
                row[1].has_value() ? std::optional<long long>{ parse_i64_default(row[1]) } : std::nullopt;
        }
    }

    std::vector<MarkerRefresh> refreshes;
    refreshes.reserve(expected.size());
    for (const std::string &marker : expected) {
        const auto source_it = source_watermark_by_marker.find(marker);
        const std::optional<long long> source_watermark =
            source_it == source_watermark_by_marker.end() ? std::nullopt : source_it->second;
        if (!existing_markers.contains(marker) || source_watermark.has_value()) {
            refreshes.push_back({ .marker = marker,
                                  .marker_exists = existing_markers.contains(marker),
                                  .source_watermark = source_watermark });
        }
    }
    return refreshes;
}

void clear_primary_rollups_for_marker(MysqlConnection &conn, UsageRangeGrain grain, std::string_view marker)
{
    const std::string table = stats_table_for_grain(grain);
    const std::string column = stat_column_for_grain(grain);
    conn.exec("DELETE FROM " + table + " WHERE " + column + "=" + conn.quote(marker));
}

void clear_scope_rollups_for_marker(MysqlConnection &conn, UsageRangeGrain grain, std::string_view marker)
{
    conn.exec("DELETE FROM " + scope_stats_table_for_grain(grain) + " WHERE " + stat_column_for_grain(grain) + "=" +
              conn.quote(marker));
}

void clear_model_rollups_for_marker(MysqlConnection &conn, UsageRangeGrain grain, std::string_view marker)
{
    conn.exec("DELETE FROM " + model_stats_table_for_grain(grain) + " WHERE " + stat_column_for_grain(grain) + "=" +
              conn.quote(marker));
}

std::string primary_rollup_select_exprs(UsageRangeGrain grain)
{
    const std::string bucket = grain == UsageRangeGrain::day  ? "DATE(`time`)" :
                               grain == UsageRangeGrain::hour ? "DATE_FORMAT(`time`,'%Y-%m-%d %H:00:00')" :
                                                                "DATE_FORMAT(`time`,'%Y-%m-%d %H:%i:00')";
    return bucket + ", user_id, token_id, COALESCE(channel_id,0), COUNT(*), "
                    "COALESCE(SUM(COALESCE(input_tokens,0)),0), "
                    "COALESCE(SUM(COALESCE(cache_read_tokens,0)),0), "
                    "COALESCE(SUM(COALESCE(cache_creation_5m_tokens,0)+COALESCE(cache_creation_1h_tokens,0)),0), "
                    "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
                    "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN 1 ELSE 0 END),0), "
                    "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN first_token_latency_ms ELSE 0 END),0), "
                    "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
                    "COALESCE(SUM(CASE WHEN latency_ms > first_token_latency_ms AND output_tokens IS NOT NULL "
                    "THEN latency_ms - first_token_latency_ms ELSE 0 END),0)";
}

std::string scope_rollup_select_exprs(UsageRangeGrain grain, std::string_view scope_type)
{
    const std::string bucket = grain == UsageRangeGrain::day  ? "DATE(`time`)" :
                               grain == UsageRangeGrain::hour ? "DATE_FORMAT(`time`,'%Y-%m-%d %H:00:00')" :
                                                                "DATE_FORMAT(`time`,'%Y-%m-%d %H:%i:00')";
    const std::string scope_id = scope_type == "user"  ? "user_id" :
                                 scope_type == "token" ? "token_id" :
                                                         "COALESCE(channel_id,0)";
    return bucket + ", " + sql_quote(scope_type) + ", " + scope_id +
           ", COUNT(*), "
           "COALESCE(SUM(COALESCE(input_tokens,0)),0), "
           "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
           "COALESCE(SUM(COALESCE(cache_read_tokens,0)),0), "
           "COALESCE(SUM(COALESCE(cache_creation_5m_tokens,0)+COALESCE(cache_creation_1h_tokens,0)),0), "
           "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN 1 ELSE 0 END),0), "
           "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN first_token_latency_ms ELSE 0 END),0), "
           "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
           "COALESCE(SUM(CASE WHEN latency_ms > first_token_latency_ms AND output_tokens IS NOT NULL "
           "THEN latency_ms - first_token_latency_ms ELSE 0 END),0)";
}

void backfill_marker(MysqlConnection &conn, UsageRangeGrain grain, std::string_view marker)
{
    const std::string table = stats_table_for_grain(grain);
    const std::string scope_table = scope_stats_table_for_grain(grain);
    const std::string model_table = model_stats_table_for_grain(grain);
    const std::string marker_table = marker_table_for_grain(grain);
    const std::string column = stat_column_for_grain(grain);

    const bool is_day = grain == UsageRangeGrain::day;
    const std::string range_start = is_day ? std::string{ marker } + " 00:00:00" : std::string{ marker };
    const std::string range_end = is_day ? format_datetime(add_days(parse_datetime(range_start), 1)) :
                                  grain == UsageRangeGrain::hour ?
                                           format_datetime(add_hours(parse_datetime(range_start), 1)) :
                                           format_datetime(add_minutes(parse_datetime(range_start), 1));

    clear_primary_rollups_for_marker(conn, grain, marker);
    clear_scope_rollups_for_marker(conn, grain, marker);
    clear_model_rollups_for_marker(conn, grain, marker);

    conn.exec("INSERT INTO " + table + " (" + column +
              ", user_id, token_id, upstream_channel_id, requests, input_tokens, cache_read_tokens, "
              "cache_creation_tokens, output_tokens, first_token_samples, "
              "first_token_latency_sum, output_tokens_for_tps, decode_latency_sum) "
              "SELECT " +
              primary_rollup_select_exprs(grain) +
              " FROM usage_events WHERE status=" + sql_quote(committed_usage_state) +
              " AND time>=" + conn.quote(range_start) + " AND time<" + conn.quote(range_end) + " GROUP BY 1,2,3,4");

    for (const std::string_view scope_type : { "user", "token", "channel" }) {
        conn.exec("INSERT INTO " + scope_table + " (" + column +
                  ", scope_type, scope_id, requests, input_tokens, output_tokens, cache_read_tokens, "
                  "cache_creation_tokens, first_token_samples, first_token_latency_sum, "
                  "output_tokens_for_tps, decode_latency_sum) "
                  "SELECT " +
                  scope_rollup_select_exprs(grain, scope_type) +
                  " FROM usage_events WHERE status=" + sql_quote(committed_usage_state) +
                  " AND time>=" + conn.quote(range_start) + " AND time<" + conn.quote(range_end) + " GROUP BY 1,2,3");
    }

    const std::string bucket = grain == UsageRangeGrain::day  ? "DATE(`time`)" :
                               grain == UsageRangeGrain::hour ? "DATE_FORMAT(`time`,'%Y-%m-%d %H:00:00')" :
                                                                "DATE_FORMAT(`time`,'%Y-%m-%d %H:%i:00')";
    conn.exec("INSERT INTO " + model_table + " (" + column +
              ", user_id, model, requests, input_tokens, output_tokens) "
              "SELECT " +
              bucket +
              ", user_id, model, COUNT(*), "
              "COALESCE(SUM(COALESCE(input_tokens,0)),0), "
              "COALESCE(SUM(COALESCE(output_tokens,0)),0) "
              "FROM usage_events WHERE status=" +
              sql_quote(committed_usage_state) +
              " AND model IS NOT NULL "
              "AND model<>'' AND time>=" +
              conn.quote(range_start) + " AND time<" + conn.quote(range_end) + " GROUP BY 1,2,3");

    conn.exec("INSERT INTO " + marker_table + " (" + column + ") VALUES (" + conn.quote(marker) +
              ") ON DUPLICATE KEY UPDATE built_at=CURRENT_TIMESTAMP");
}

UsageTotals query_rollup_primary(MysqlConnection &conn, UsageRangeGrain grain, std::string_view start_utc,
                                 std::string_view end_utc, const UsagePrimaryFilter &filter)
{
    UsageTotals totals;
    if (start_utc >= end_utc) {
        return totals;
    }
    const std::string sql =
        "SELECT COALESCE(SUM(requests),0), COALESCE(SUM(input_tokens),0), "
        "COALESCE(SUM(cache_read_tokens),0), COALESCE(SUM(cache_creation_tokens),0), "
        "COALESCE(SUM(output_tokens),0), "
        "COALESCE(SUM(first_token_samples),0), COALESCE(SUM(first_token_latency_sum),0), "
        "COALESCE(SUM(output_tokens_for_tps),0), COALESCE(SUM(decode_latency_sum),0) FROM " +
        stats_table_for_grain(grain) + " WHERE " + stat_column_for_grain(grain) + ">=" +
        conn.quote(grain == UsageRangeGrain::day ? std::string{ start_utc }.substr(0, 10) : std::string{ start_utc }) +
        " AND " + stat_column_for_grain(grain) + "<" +
        conn.quote(grain == UsageRangeGrain::day ? std::string{ end_utc }.substr(0, 10) : std::string{ end_utc }) +
        sql_primary_filter(filter, true);
    const auto rows = conn.query_rows(sql);
    if (!rows.empty()) {
        add_usage_totals_from_row(totals, rows[0]);
    }
    return totals;
}

UsageTotals query_raw_primary(MysqlConnection &conn, std::string_view start_utc, std::string_view end_utc,
                              const UsagePrimaryFilter &filter)
{
    UsageTotals totals;
    const std::string sql =
        "SELECT COUNT(*), COALESCE(SUM(COALESCE(input_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_read_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_creation_5m_tokens,0)+COALESCE(cache_creation_1h_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN 1 ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN first_token_latency_ms ELSE 0 END),0), "
        "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
        "COALESCE(SUM(CASE WHEN latency_ms > first_token_latency_ms AND output_tokens IS NOT NULL "
        "THEN latency_ms - first_token_latency_ms ELSE 0 END),0) "
        "FROM usage_events WHERE status=" +
        sql_quote(committed_usage_state) + " AND time>=" + conn.quote(start_utc) + " AND time<" + conn.quote(end_utc) +
        sql_primary_filter(filter);
    const auto rows = conn.query_rows(sql);
    if (!rows.empty()) {
        add_usage_totals_from_row(totals, rows[0]);
    }
    return totals;
}

UsageTotals query_rollup_scope(MysqlConnection &conn, UsageRangeGrain grain, std::string_view start_utc,
                               std::string_view end_utc, std::string_view scope_type, long long scope_id)
{
    UsageTotals totals;
    const std::string sql =
        "SELECT COALESCE(SUM(requests),0), COALESCE(SUM(input_tokens),0), "
        "COALESCE(SUM(cache_read_tokens),0), COALESCE(SUM(cache_creation_tokens),0), "
        "COALESCE(SUM(output_tokens),0), "
        "COALESCE(SUM(first_token_samples),0), COALESCE(SUM(first_token_latency_sum),0), "
        "COALESCE(SUM(output_tokens_for_tps),0), COALESCE(SUM(decode_latency_sum),0) FROM " +
        scope_stats_table_for_grain(grain) + " WHERE " + stat_column_for_grain(grain) + ">=" +
        conn.quote(grain == UsageRangeGrain::day ? std::string{ start_utc }.substr(0, 10) : std::string{ start_utc }) +
        " AND " + stat_column_for_grain(grain) + "<" +
        conn.quote(grain == UsageRangeGrain::day ? std::string{ end_utc }.substr(0, 10) : std::string{ end_utc }) +
        " AND scope_type=" + conn.quote(scope_type) + " AND scope_id=" + std::to_string(scope_id);
    const auto rows = conn.query_rows(sql);
    if (!rows.empty()) {
        add_usage_totals_from_row(totals, rows[0]);
    }
    return totals;
}

UsageTotals query_raw_scope(MysqlConnection &conn, std::string_view start_utc, std::string_view end_utc,
                            std::string_view scope_type, long long scope_id)
{
    UsagePrimaryFilter filter;
    if (scope_type == "user") {
        filter.user_id = scope_id;
    } else if (scope_type == "token") {
        filter.token_id = scope_id;
    } else if (scope_type == "channel") {
        filter.channel_id = scope_id;
    } else {
        assert(false && "internal: unsupported scope_type");
        return {};
    }
    return query_raw_primary(conn, start_utc, end_utc, filter);
}

UsageTotals query_rollup_model(MysqlConnection &conn, UsageRangeGrain grain, std::string_view start_utc,
                               std::string_view end_utc, long long user_id, std::string_view model)
{
    UsageTotals totals;
    const std::string sql =
        "SELECT COALESCE(SUM(requests),0), COALESCE(SUM(input_tokens),0), 0, 0, "
        "COALESCE(SUM(output_tokens),0), 0, 0, 0, 0 FROM " +
        model_stats_table_for_grain(grain) + " WHERE " + stat_column_for_grain(grain) + ">=" +
        conn.quote(grain == UsageRangeGrain::day ? std::string{ start_utc }.substr(0, 10) : std::string{ start_utc }) +
        " AND " + stat_column_for_grain(grain) + "<" +
        conn.quote(grain == UsageRangeGrain::day ? std::string{ end_utc }.substr(0, 10) : std::string{ end_utc }) +
        " AND user_id=" + std::to_string(user_id) + " AND model=" + conn.quote(model);
    const auto rows = conn.query_rows(sql);
    if (!rows.empty()) {
        add_usage_totals_from_row(totals, rows[0]);
    }
    return totals;
}

UsageTotals query_raw_model(MysqlConnection &conn, std::string_view start_utc, std::string_view end_utc,
                            long long user_id, std::string_view model)
{
    UsageTotals totals;
    const std::string sql = "SELECT COUNT(*), COALESCE(SUM(COALESCE(input_tokens,0)),0), 0, 0, "
                            "COALESCE(SUM(COALESCE(output_tokens,0)),0), 0, 0, 0, 0 "
                            "FROM usage_events WHERE status=" +
                            sql_quote(committed_usage_state) + " AND time>=" + conn.quote(start_utc) + " AND time<" +
                            conn.quote(end_utc) + " AND user_id=" + std::to_string(user_id) +
                            " AND model=" + conn.quote(model);
    const auto rows = conn.query_rows(sql);
    if (!rows.empty()) {
        add_usage_totals_from_row(totals, rows[0]);
    }
    return totals;
}

void merge_usage_totals(UsageTotals &base, const UsageTotals &add)
{
    base.requests += add.requests;
    base.input_tokens += add.input_tokens;
    base.cache_read_tokens += add.cache_read_tokens;
    base.cache_creation_tokens += add.cache_creation_tokens;
    base.output_tokens += add.output_tokens;
    base.committed_usd_micros += add.committed_usd_micros;
    base.first_token_samples += add.first_token_samples;
    base.first_token_latency_sum += add.first_token_latency_sum;
    base.output_tokens_for_tps += add.output_tokens_for_tps;
    base.decode_latency_sum += add.decode_latency_sum;
}

} // namespace

std::string usage_range_grain_name(UsageRangeGrain grain)
{
    switch (grain) {
    case UsageRangeGrain::raw:
        return "raw";
    case UsageRangeGrain::minute:
        return "minute";
    case UsageRangeGrain::hour:
        return "hour";
    case UsageRangeGrain::day:
        return "day";
    }
    assert(false && "internal: unknown grain");
    return {};
}

UsageAggregationStore::UsageAggregationStore(MysqlConnection &conn)
    : conn_(conn)
{
}

std::uint64_t UsageAggregationStore::coverage_epoch() const
{
    return coverage_epoch_;
}

void UsageAggregationStore::invalidate_coverage_for_time(std::string_view time_utc)
{
    const DateTimeParts time = parse_datetime(time_utc);
    conn_.exec("DELETE FROM usage_daily_stats_backfilled WHERE stat_date=" + conn_.quote(format_date(time)));
    conn_.exec("DELETE FROM usage_hourly_stats_backfilled WHERE stat_hour=" +
               conn_.quote(format_datetime(floor_to_hour(time))));
    conn_.exec("DELETE FROM usage_minute_stats_backfilled WHERE stat_minute=" +
               conn_.quote(format_datetime(floor_to_minute(time))));
    ++coverage_epoch_;
    coverage_cache_.clear();
}

void UsageAggregationStore::ensure_daily_coverage(std::string_view start_utc, std::string_view end_utc)
{
    const CoverageWindow window = to_coverage_window(UsageRangeGrain::day, start_utc, end_utc);
    if (window.start_utc.empty()) {
        return;
    }
    const auto refreshes = markers_to_refresh(conn_, UsageRangeGrain::day, window.start_utc, window.end_utc);
    std::optional<long long> source_watermark;
    for (const MarkerRefresh &refresh : refreshes) {
        if (refresh.source_watermark.has_value() &&
            (!source_watermark.has_value() || *source_watermark < *refresh.source_watermark)) {
            source_watermark = refresh.source_watermark;
        }
    }
    const std::string cache_key = coverage_cache_key(UsageRangeGrain::day, window.start_utc, window.end_utc);
    if (cache_contains(coverage_cache_, cache_key, coverage_epoch_, source_watermark)) {
        return;
    }
    for (const MarkerRefresh &refresh : refreshes) {
        backfill_marker(conn_, UsageRangeGrain::day, refresh.marker);
    }
    cache_insert(coverage_cache_, cache_key, coverage_epoch_, source_watermark);
}

void UsageAggregationStore::ensure_hourly_coverage(std::string_view start_utc, std::string_view end_utc)
{
    const CoverageWindow window = to_coverage_window(UsageRangeGrain::hour, start_utc, end_utc);
    if (window.start_utc.empty()) {
        return;
    }
    const auto refreshes = markers_to_refresh(conn_, UsageRangeGrain::hour, window.start_utc, window.end_utc);
    std::optional<long long> source_watermark;
    for (const MarkerRefresh &refresh : refreshes) {
        if (refresh.source_watermark.has_value() &&
            (!source_watermark.has_value() || *source_watermark < *refresh.source_watermark)) {
            source_watermark = refresh.source_watermark;
        }
    }
    const std::string cache_key = coverage_cache_key(UsageRangeGrain::hour, window.start_utc, window.end_utc);
    if (cache_contains(coverage_cache_, cache_key, coverage_epoch_, source_watermark)) {
        return;
    }
    for (const MarkerRefresh &refresh : refreshes) {
        backfill_marker(conn_, UsageRangeGrain::hour, refresh.marker);
    }
    cache_insert(coverage_cache_, cache_key, coverage_epoch_, source_watermark);
}

void UsageAggregationStore::ensure_minute_coverage(std::string_view start_utc, std::string_view end_utc)
{
    const CoverageWindow window = to_coverage_window(UsageRangeGrain::minute, start_utc, end_utc);
    if (window.start_utc.empty()) {
        return;
    }
    const auto refreshes = markers_to_refresh(conn_, UsageRangeGrain::minute, window.start_utc, window.end_utc);
    std::optional<long long> source_watermark;
    for (const MarkerRefresh &refresh : refreshes) {
        if (refresh.source_watermark.has_value() &&
            (!source_watermark.has_value() || *source_watermark < *refresh.source_watermark)) {
            source_watermark = refresh.source_watermark;
        }
    }
    const std::string cache_key = coverage_cache_key(UsageRangeGrain::minute, window.start_utc, window.end_utc);
    if (cache_contains(coverage_cache_, cache_key, coverage_epoch_, source_watermark)) {
        return;
    }
    for (const MarkerRefresh &refresh : refreshes) {
        backfill_marker(conn_, UsageRangeGrain::minute, refresh.marker);
    }
    cache_insert(coverage_cache_, cache_key, coverage_epoch_, source_watermark);
}

std::vector<UsageRangeSegment> UsageAggregationStore::split_query_range(std::string_view start_utc,
                                                                        std::string_view end_utc) const
{
    const DateTimeParts start = parse_datetime(start_utc);
    const DateTimeParts end = parse_datetime(end_utc);
    if (format_datetime(start) >= format_datetime(end)) {
        return {};
    }

    std::vector<UsageRangeSegment> out;
    DateTimeParts cursor = start;
    const DateTimeParts hour_aligned_end = floor_to_hour(end);
    const DateTimeParts minute_aligned_end = floor_to_minute(end);

    if (cursor.second != 0) {
        const std::string seg_end =
            std::min(format_datetime(add_minutes(floor_to_minute(cursor), 1)), format_datetime(end));
        out.push_back({ .grain = UsageRangeGrain::raw, .start_utc = format_datetime(cursor), .end_utc = seg_end });
        cursor = parse_datetime(seg_end);
    }

    if ((cursor.minute != 0 || cursor.second != 0) && format_datetime(cursor) < format_datetime(end)) {
        const std::string seg_end =
            std::min(format_datetime(add_hours(floor_to_hour(cursor), 1)), format_datetime(minute_aligned_end));
        if (format_datetime(cursor) < seg_end) {
            out.push_back(
                { .grain = UsageRangeGrain::minute, .start_utc = format_datetime(cursor), .end_utc = seg_end });
            cursor = parse_datetime(seg_end);
        }
    }

    if (cursor.hour != 0 && format_datetime(cursor) < format_datetime(hour_aligned_end)) {
        const std::string seg_end =
            std::min(format_datetime(add_days(floor_to_day(cursor), 1)), format_datetime(hour_aligned_end));
        if (format_datetime(cursor) < seg_end) {
            out.push_back({ .grain = UsageRangeGrain::hour, .start_utc = format_datetime(cursor), .end_utc = seg_end });
            cursor = parse_datetime(seg_end);
        }
    }

    const DateTimeParts day_aligned_end = floor_to_day(end);
    if (format_datetime(add_days(cursor, 1)) <= format_datetime(day_aligned_end)) {
        DateTimeParts run_end = add_days(cursor, 1);
        while (format_datetime(add_days(run_end, 1)) <= format_datetime(day_aligned_end)) {
            run_end = add_days(run_end, 1);
        }
        out.push_back({ .grain = UsageRangeGrain::day,
                        .start_utc = format_datetime(cursor),
                        .end_utc = format_datetime(run_end) });
        cursor = run_end;
    }

    if (cursor.minute == 0 && cursor.second == 0 && format_datetime(cursor) < format_datetime(hour_aligned_end)) {
        out.push_back({ .grain = UsageRangeGrain::hour,
                        .start_utc = format_datetime(cursor),
                        .end_utc = format_datetime(hour_aligned_end) });
        cursor = hour_aligned_end;
    }

    if (cursor.second == 0 && format_datetime(cursor) < format_datetime(minute_aligned_end)) {
        out.push_back({ .grain = UsageRangeGrain::minute,
                        .start_utc = format_datetime(cursor),
                        .end_utc = format_datetime(minute_aligned_end) });
        cursor = minute_aligned_end;
    }

    if (format_datetime(cursor) < format_datetime(end)) {
        out.push_back(
            { .grain = UsageRangeGrain::raw, .start_utc = format_datetime(cursor), .end_utc = format_datetime(end) });
    }

    std::vector<UsageRangeSegment> compact;
    for (const UsageRangeSegment &segment : out) {
        if (segment.start_utc >= segment.end_utc) {
            continue;
        }
        if (!compact.empty() && compact.back().grain == segment.grain && compact.back().end_utc == segment.start_utc) {
            compact.back().end_utc = segment.end_utc;
            continue;
        }
        compact.push_back(segment);
    }
    return compact;
}

UsageTotals UsageAggregationStore::sum_primary(std::string_view start_utc, std::string_view end_utc,
                                               const UsagePrimaryFilter &filter)
{
    const auto segments = split_query_range(start_utc, end_utc);
    for (const UsageRangeSegment &segment : segments) {
        if (segment.grain == UsageRangeGrain::day) {
            ensure_daily_coverage(segment.start_utc, segment.end_utc);
        } else if (segment.grain == UsageRangeGrain::hour) {
            ensure_hourly_coverage(segment.start_utc, segment.end_utc);
        } else if (segment.grain == UsageRangeGrain::minute) {
            ensure_minute_coverage(segment.start_utc, segment.end_utc);
        }
    }
    UsageTotals totals;
    for (const UsageRangeSegment &segment : segments) {
        UsageTotals part;
        switch (segment.grain) {
        case UsageRangeGrain::day:
            part = query_rollup_primary(conn_, UsageRangeGrain::day, segment.start_utc, segment.end_utc, filter);
            break;
        case UsageRangeGrain::hour:
            part = query_rollup_primary(conn_, UsageRangeGrain::hour, segment.start_utc, segment.end_utc, filter);
            break;
        case UsageRangeGrain::minute:
            part = query_rollup_primary(conn_, UsageRangeGrain::minute, segment.start_utc, segment.end_utc, filter);
            break;
        case UsageRangeGrain::raw:
            part = query_raw_primary(conn_, segment.start_utc, segment.end_utc, filter);
            break;
        }
        merge_usage_totals(totals, part);
    }
    return totals;
}

UsageTotals UsageAggregationStore::sum_scope(std::string_view start_utc, std::string_view end_utc,
                                             std::string_view scope_type, long long scope_id)
{
    const auto normalized_scope_type = normalize_live_scope_type(scope_type);
    if (!normalized_scope_type.has_value()) {
        throw std::invalid_argument("unsupported scope_type");
    }
    const auto segments = split_query_range(start_utc, end_utc);
    for (const UsageRangeSegment &segment : segments) {
        if (segment.grain == UsageRangeGrain::day) {
            ensure_daily_coverage(segment.start_utc, segment.end_utc);
        } else if (segment.grain == UsageRangeGrain::hour) {
            ensure_hourly_coverage(segment.start_utc, segment.end_utc);
        } else if (segment.grain == UsageRangeGrain::minute) {
            ensure_minute_coverage(segment.start_utc, segment.end_utc);
        }
    }
    UsageTotals totals;
    for (const UsageRangeSegment &segment : segments) {
        UsageTotals part;
        switch (segment.grain) {
        case UsageRangeGrain::day:
            part = query_rollup_scope(conn_, UsageRangeGrain::day, segment.start_utc, segment.end_utc,
                                      *normalized_scope_type, scope_id);
            break;
        case UsageRangeGrain::hour:
            part = query_rollup_scope(conn_, UsageRangeGrain::hour, segment.start_utc, segment.end_utc,
                                      *normalized_scope_type, scope_id);
            break;
        case UsageRangeGrain::minute:
            part = query_rollup_scope(conn_, UsageRangeGrain::minute, segment.start_utc, segment.end_utc,
                                      *normalized_scope_type, scope_id);
            break;
        case UsageRangeGrain::raw:
            part = query_raw_scope(conn_, segment.start_utc, segment.end_utc, *normalized_scope_type, scope_id);
            break;
        }
        merge_usage_totals(totals, part);
    }
    return totals;
}

UsageTotals UsageAggregationStore::sum_model(std::string_view start_utc, std::string_view end_utc, long long user_id,
                                             std::string_view model)
{
    const auto segments = split_query_range(start_utc, end_utc);
    for (const UsageRangeSegment &segment : segments) {
        if (segment.grain == UsageRangeGrain::day) {
            ensure_daily_coverage(segment.start_utc, segment.end_utc);
        } else if (segment.grain == UsageRangeGrain::hour) {
            ensure_hourly_coverage(segment.start_utc, segment.end_utc);
        } else if (segment.grain == UsageRangeGrain::minute) {
            ensure_minute_coverage(segment.start_utc, segment.end_utc);
        }
    }
    UsageTotals totals;
    for (const UsageRangeSegment &segment : segments) {
        UsageTotals part;
        switch (segment.grain) {
        case UsageRangeGrain::day:
            part = query_rollup_model(conn_, UsageRangeGrain::day, segment.start_utc, segment.end_utc, user_id, model);
            break;
        case UsageRangeGrain::hour:
            part = query_rollup_model(conn_, UsageRangeGrain::hour, segment.start_utc, segment.end_utc, user_id, model);
            break;
        case UsageRangeGrain::minute:
            part =
                query_rollup_model(conn_, UsageRangeGrain::minute, segment.start_utc, segment.end_utc, user_id, model);
            break;
        case UsageRangeGrain::raw:
            part = query_raw_model(conn_, segment.start_utc, segment.end_utc, user_id, model);
            break;
        }
        merge_usage_totals(totals, part);
    }
    return totals;
}

} // namespace revlm
