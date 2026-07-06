#include "usage/usage.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

constexpr int default_usage_event_limit = 100;
constexpr int max_usage_event_limit = 500;
constexpr int default_admin_usage_limit = 50;
constexpr int max_admin_usage_limit = 200;
constexpr int default_suggest_limit = 20;
constexpr int max_suggest_limit = 50;
constexpr size_t service_tier_max_len = 32;
constexpr size_t downgrade_reason_max_len = 64;
constexpr size_t endpoint_max_len = 128;
constexpr size_t method_max_len = 16;
constexpr size_t error_class_max_len = 64;
constexpr size_t error_message_max_len = 255;
constexpr size_t model_max_len = 128;
constexpr size_t price_group_name_max_len = 255;
constexpr size_t decimal_scale = 6;
constexpr size_t decimal_integer_digits = 19;

bool parse_i64(std::string_view raw, long long &out)
{
    const std::string trimmed = trim_ascii(raw);
    if (trimmed.empty()) {
        return false;
    }
    bool negative = false;
    size_t pos = 0;
    if (trimmed[0] == '-') {
        negative = true;
        pos = 1;
    }
    if (pos >= trimmed.size()) {
        return false;
    }
    long long value = 0;
    for (; pos < trimmed.size(); ++pos) {
        const char ch = trimmed[pos];
        if (ch < '0' || ch > '9') {
            return false;
        }
        const int digit = ch - '0';
        if (value > (std::numeric_limits<long long>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = negative ? -value : value;
    return true;
}

long long parse_i64_or_zero(const std::optional<std::string> &raw)
{
    if (!raw.has_value()) {
        return 0;
    }
    long long value = 0;
    return parse_i64(*raw, value) ? value : 0;
}

std::optional<long long> parse_optional_i64(const std::optional<std::string> &raw)
{
    if (!raw.has_value()) {
        return std::nullopt;
    }
    long long value = 0;
    if (!parse_i64(*raw, value)) {
        return std::nullopt;
    }
    return value;
}

int parse_int_or_zero(const std::optional<std::string> &raw)
{
    const long long value = parse_i64_or_zero(raw);
    if (value < std::numeric_limits<int>::min()) {
        return std::numeric_limits<int>::min();
    }
    if (value > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(value);
}

double parse_double_or_zero(const std::optional<std::string> &raw)
{
    if (!raw.has_value()) {
        return 0.0;
    }
    try {
        return std::stod(trim_ascii(*raw));
    } catch (const std::exception &) {
        return 0.0;
    }
}

std::string optional_string(const std::optional<std::string> &raw)
{
    return raw.has_value() ? trim_ascii(*raw) : std::string{};
}

std::optional<std::string> optional_trimmed(const std::optional<std::string> &raw)
{
    if (!raw.has_value()) {
        return std::nullopt;
    }
    std::string trimmed = trim_ascii(*raw);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    return trimmed;
}

std::optional<long long> parse_non_negative_i64(std::string_view raw)
{
    const std::string trimmed = trim_ascii(raw);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    long long value = 0;
    for (char ch : trimmed) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        const int digit = ch - '0';
        if (value > (std::numeric_limits<long long>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

std::optional<std::string> nullable_trimmed_limited(const std::optional<std::string> &value, size_t max_len)
{
    if (!value.has_value()) {
        return std::nullopt;
    }
    std::string trimmed = trim_ascii(*value);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    if (trimmed.size() > max_len) {
        trimmed.resize(max_len);
    }
    return trimmed;
}

bool is_duplicate_request_id_error(const MysqlConnection &conn)
{
    return conn.last_errno() == 1062;
}

std::optional<std::string> normalize_model_text(const std::optional<std::string> &value)
{
    return nullable_trimmed_limited(value, model_max_len);
}

int normalize_status_code(int status_code)
{
    if (status_code < 0 || status_code > 999) {
        return 0;
    }
    return status_code;
}

int normalize_non_negative_int(int value)
{
    return value < 0 ? 0 : value;
}

long long normalize_non_negative_i64(long long value)
{
    return value < 0 ? 0 : value;
}

bool parse_boolish(std::string_view raw)
{
    const std::string trimmed = trim_ascii(raw);
    return trimmed == "1" || trimmed == "true" || trimmed == "TRUE";
}

std::optional<std::string> row_string(const MysqlResultRow &row, size_t idx, size_t max_len)
{
    if (idx >= row.size() || !row[idx].has_value()) {
        return std::nullopt;
    }
    std::string value = trim_ascii(*row[idx]);
    if (value.empty()) {
        return std::nullopt;
    }
    if (value.size() > max_len) {
        value.resize(max_len);
    }
    return value;
}

std::optional<long long> row_i64(const MysqlResultRow &row, size_t idx)
{
    if (idx >= row.size() || !row[idx].has_value()) {
        return std::nullopt;
    }
    return parse_non_negative_i64(*row[idx]);
}

std::string select_usage_event_sql(std::string_view where_clause)
{
    return std::string{ "SELECT id,request_id,user_id,token_id,state,model,endpoint,method,status_code,"
                        "latency_ms,first_token_latency_ms,error_class,error_message,forwarded_model,"
                        "upstream_response_model,channel_id,"
                        "requested_service_tier,service_tier,"
                        "service_tier_downgrade_reason,input_tokens,cache_read_input_tokens,"
                        "cache_creation_input_tokens,cache_creation_1h_input_tokens,output_tokens,"
                        "committed_usd,price_multiplier,price_multiplier_group,"
                        "price_multiplier_payment,price_multiplier_group_name,is_stream,"
                        "request_bytes,response_bytes "
                        "FROM usage_events " } +
           std::string{ where_clause };
}

int normalize_usage_event_limit(int limit, bool admin)
{
    if (admin) {
        if (limit <= 0) {
            return default_admin_usage_limit;
        }
        return std::min(limit, max_admin_usage_limit);
    }
    if (limit <= 0) {
        return default_usage_event_limit;
    }
    return std::min(limit, max_usage_event_limit);
}

int normalize_suggest_limit(int limit)
{
    if (limit <= 0) {
        return default_suggest_limit;
    }
    return std::min(limit, max_suggest_limit);
}

std::string build_like_pattern(std::string_view raw)
{
    const std::string trimmed = trim_ascii(raw);
    if (trimmed.empty()) {
        return {};
    }
    if (trimmed.find_first_of("%_") != std::string::npos) {
        return trimmed;
    }
    return trimmed + "%";
}

double compute_cache_ratio(long long tokens, long long cache_read, long long cache_creation)
{
    if (tokens <= 0) {
        return 0.0;
    }
    return static_cast<double>(cache_read + cache_creation) / static_cast<double>(tokens);
}

double compute_tokens_per_second(long long output_tokens, long long total_latency_ms, long long first_token_latency_ms)
{
    if (output_tokens <= 0) {
        return 0.0;
    }
    const long long decode_latency_ms = total_latency_ms - first_token_latency_ms;
    if (decode_latency_ms <= 0) {
        return 0.0;
    }
    return static_cast<double>(output_tokens) * 1000.0 / static_cast<double>(decode_latency_ms);
}

struct UsageEventQueryParts {
    std::string joins;
    std::string where;
};

std::optional<UsageEvent> row_to_usage_event(const MysqlResultRow &row)
{
    if (row.size() < 35 || !row[0].has_value()) {
        return std::nullopt;
    }
    UsageEvent event;
    event.id = parse_i64_or_zero(row[0]);
    event.time = row[1].value_or("");
    event.request_id = row[2].value_or("");
    event.endpoint = optional_trimmed(row[3]);
    event.method = optional_trimmed(row[4]);
    event.user_id = parse_i64_or_zero(row[5]);
    event.token_id = parse_i64_or_zero(row[6]);
    event.channel_id = parse_optional_i64(row[7]);
    event.state = row[8].value_or("");
    event.model = optional_trimmed(row[9]);
    event.forwarded_model = optional_trimmed(row[10]);
    event.upstream_response_model = optional_trimmed(row[11]);
    event.requested_service_tier = optional_trimmed(row[12]);
    event.service_tier = optional_trimmed(row[13]);
    event.service_tier_downgrade_reason = optional_trimmed(row[14]);
    event.input_tokens = parse_optional_i64(row[15]);
    event.cache_read_input_tokens = parse_optional_i64(row[16]);
    event.cache_creation_input_tokens = parse_optional_i64(row[17]);
    event.cache_creation_1h_input_tokens = parse_optional_i64(row[18]);
    event.output_tokens = parse_optional_i64(row[19]);
    event.committed_usd = trim_ascii(row[20].value_or("0"));
    if (event.committed_usd.empty()) {
        event.committed_usd = "0";
    }
    event.price_multiplier = optional_trimmed(row[21]);
    event.price_multiplier_group = optional_trimmed(row[22]);
    event.price_multiplier_payment = optional_trimmed(row[23]);
    event.price_multiplier_group_name = optional_trimmed(row[24]);
    event.status_code = parse_int_or_zero(row[25]);
    event.latency_ms = parse_int_or_zero(row[26]);
    event.first_token_latency_ms = parse_int_or_zero(row[27]);
    event.error_class = optional_trimmed(row[28]);
    event.error_message = optional_trimmed(row[29]);
    event.is_stream = parse_i64_or_zero(row[30]) != 0;
    event.request_bytes = parse_i64_or_zero(row[31]);
    event.response_bytes = parse_i64_or_zero(row[32]);
    event.created_at = row[33].value_or("");
    event.updated_at = row[34].value_or("");
    return event;
}

std::string usage_event_columns(std::string_view prefix)
{
    return std::string{ prefix } + "id, " + std::string{ prefix } + "time, " + std::string{ prefix } + "request_id, " +
           std::string{ prefix } + "endpoint, " + std::string{ prefix } + "method, " + std::string{ prefix } +
           "user_id, " + std::string{ prefix } + "token_id, " + std::string{ prefix } + "channel_id, " +
           std::string{ prefix } + "state, " + std::string{ prefix } + "model, " + std::string{ prefix } +
           "forwarded_model, " + std::string{ prefix } + "upstream_response_model, " + std::string{ prefix } +
           "requested_service_tier, " + std::string{ prefix } + "service_tier, " + std::string{ prefix } +
           "service_tier_downgrade_reason, " + std::string{ prefix } + "input_tokens, " + std::string{ prefix } +
           "cache_read_input_tokens, " + std::string{ prefix } + "cache_creation_input_tokens, " +
           std::string{ prefix } + "cache_creation_1h_input_tokens, " + std::string{ prefix } + "output_tokens, " +
           std::string{ prefix } + "committed_usd, " + std::string{ prefix } + "price_multiplier, " +
           std::string{ prefix } + "price_multiplier_group, " + std::string{ prefix } + "price_multiplier_payment, " +
           std::string{ prefix } + "price_multiplier_group_name, " + std::string{ prefix } + "status_code, " +
           std::string{ prefix } + "latency_ms, " + std::string{ prefix } + "first_token_latency_ms, " +
           std::string{ prefix } + "error_class, " + std::string{ prefix } + "error_message, " + std::string{ prefix } +
           "is_stream, " + std::string{ prefix } + "request_bytes, " + std::string{ prefix } + "response_bytes, " +
           std::string{ prefix } + "created_at, " + std::string{ prefix } + "updated_at";
}

std::vector<UsageEvent> scan_usage_events(const std::vector<MysqlResultRow> &rows)
{
    std::vector<UsageEvent> out;
    out.reserve(rows.size());
    for (const MysqlResultRow &row : rows) {
        if (auto event = row_to_usage_event(row); event.has_value()) {
            out.push_back(std::move(*event));
        }
    }
    return out;
}

std::vector<UsageEventWithUser> scan_usage_events_with_user(const std::vector<MysqlResultRow> &rows)
{
    std::vector<UsageEventWithUser> out;
    out.reserve(rows.size());
    for (const MysqlResultRow &row : rows) {
        if (row.size() < 37) {
            continue;
        }
        std::vector<MysqlResultRow> head{ MysqlResultRow(row.begin(), row.begin() + 36) };
        auto events = scan_usage_events(head);
        if (events.empty()) {
            continue;
        }
        UsageEventWithUser item;
        item.event = std::move(events[0]);
        item.user_email = row[36].value_or("");
        out.push_back(std::move(item));
    }
    return out;
}

UsageTokenStats scan_usage_token_stats_row(const MysqlResultRow &row)
{
    UsageTokenStats stats;
    if (row.empty()) {
        return stats;
    }
    stats.requests = parse_i64_or_zero(row[0]);
    stats.input_tokens = row.size() > 1 ? parse_i64_or_zero(row[1]) : 0;
    stats.output_tokens = row.size() > 2 ? parse_i64_or_zero(row[2]) : 0;
    stats.cache_read_input_tokens = row.size() > 3 ? parse_i64_or_zero(row[3]) : 0;
    stats.cache_creation_input_tokens = row.size() > 4 ? parse_i64_or_zero(row[4]) : 0;
    stats.committed_usd = row.size() > 5 ? trim_ascii(row[5].value_or("0")) : "0";
    if (stats.committed_usd.empty()) {
        stats.committed_usd = "0";
    }
    const long long first_token_latency_sum = row.size() > 6 ? parse_i64_or_zero(row[6]) : 0;
    stats.first_token_samples = row.size() > 7 ? parse_i64_or_zero(row[7]) : 0;
    const long long total_latency_sum = row.size() > 8 ? parse_i64_or_zero(row[8]) : 0;
    stats.tokens = stats.input_tokens + stats.output_tokens;
    stats.cache_ratio =
        compute_cache_ratio(stats.tokens, stats.cache_read_input_tokens, stats.cache_creation_input_tokens);
    if (stats.first_token_samples > 0) {
        stats.avg_first_token_ms =
            static_cast<double>(first_token_latency_sum) / static_cast<double>(stats.first_token_samples);
    }
    stats.output_tokens_per_sec =
        compute_tokens_per_second(stats.output_tokens, total_latency_sum, first_token_latency_sum);
    return stats;
}

std::string normalize_granularity(std::string_view raw)
{
    const std::string value = lowercase_ascii(trim_ascii(raw));
    if (value.empty() || value == "hour") {
        return "hour";
    }
    if (value == "day") {
        return "day";
    }
    assert(false && "internal: granularity must be hour or day");
    return "hour";
}

std::string usage_bucket_expr(std::string_view granularity, std::string_view column)
{
    if (granularity == "day") {
        return "DATE_FORMAT(" + std::string{ column } + ", '%Y-%m-%d 00:00')";
    }
    return "DATE_FORMAT(" + std::string{ column } + ", '%Y-%m-%d %H:00')";
}

std::vector<UsageTimeSeriesPoint> scan_usage_time_series(const std::vector<MysqlResultRow> &rows)
{
    std::vector<UsageTimeSeriesPoint> out;
    out.reserve(rows.size());
    for (const MysqlResultRow &row : rows) {
        if (row.size() < 8) {
            continue;
        }
        UsageTimeSeriesPoint point;
        point.bucket = row[0].value_or("");
        point.requests = parse_i64_or_zero(row[1]);
        const long long input_tokens = parse_i64_or_zero(row[2]);
        const long long output_tokens = parse_i64_or_zero(row[3]);
        const long long cache_read = parse_i64_or_zero(row[4]);
        const long long cache_create = parse_i64_or_zero(row[5]);
        const long long first_token_sum = parse_i64_or_zero(row[6]);
        const long long first_token_samples = parse_i64_or_zero(row[7]);
        const long long total_latency_sum = row.size() > 8 ? parse_i64_or_zero(row[8]) : 0;
        point.tokens = input_tokens + output_tokens;
        point.committed_usd = row.size() > 9 ? parse_double_or_zero(row[9]) : 0.0;
        point.cache_ratio = compute_cache_ratio(point.tokens, cache_read, cache_create) * 100.0;
        if (first_token_samples > 0) {
            point.avg_first_token_latency =
                static_cast<double>(first_token_sum) / static_cast<double>(first_token_samples);
        }
        point.tokens_per_second = compute_tokens_per_second(output_tokens, total_latency_sum, first_token_sum);
        out.push_back(std::move(point));
    }
    return out;
}

std::string sanitize_query_input(std::string_view raw)
{
    return trim_ascii(raw);
}

UsageEventQueryParts build_usage_event_filter_sql(MysqlConnection &conn, const UsageEventsIndexFlags &index_flags,
                                                  const UsageEventsFilters &filters, bool include_user_join)
{
    UsageEventQueryParts parts;
    if (index_flags.key && !filters.key.empty()) {
        parts.joins += " LEFT JOIN user_tokens ut ON ut.id=ue.token_id";
        parts.where += " AND ut.name LIKE " + conn.quote(build_like_pattern(filters.key));
    }
    if (filters.channel_id.has_value() && *filters.channel_id > 0) {
        parts.where += " AND ue.channel_id=" + std::to_string(*filters.channel_id);
    } else if (index_flags.channel && !filters.channel.empty()) {
        long long channel_id = 0;
        if (parse_i64(filters.channel, channel_id) && channel_id > 0) {
            parts.where += " AND ue.channel_id=" + std::to_string(channel_id);
        } else {
            parts.joins += " LEFT JOIN channels uc ON uc.id=ue.channel_id";
            parts.where += " AND uc.name LIKE " + conn.quote(build_like_pattern(filters.channel));
        }
    }
    if (filters.model_exact.has_value() && !filters.model_exact->empty()) {
        parts.where += " AND ue.model=" + conn.quote(*filters.model_exact);
    } else if (index_flags.model && !filters.model.empty()) {
        parts.where += " AND ue.model LIKE " + conn.quote(build_like_pattern(filters.model));
    }
    if (filters.user_id.has_value() && *filters.user_id > 0) {
        parts.where += " AND ue.user_id=" + std::to_string(*filters.user_id);
    } else if (include_user_join && index_flags.user && !filters.user.empty()) {
        parts.where += " AND (u.email LIKE " + conn.quote(build_like_pattern(filters.user)) + " OR u.username LIKE " +
                       conn.quote(build_like_pattern(filters.user)) + ")";
    }
    return parts;
}

void append_usage_event_cursor(std::string &sql, const std::optional<long long> &before_id,
                               const std::optional<long long> &after_id)
{
    if (before_id.has_value() && after_id.has_value()) {
        throw std::invalid_argument("before_id and after_id cannot be used together");
    }
    if (before_id.has_value()) {
        if (*before_id <= 0) {
            throw std::invalid_argument("before_id must be positive");
        }
        sql += " AND ue.id < " + std::to_string(*before_id);
    }
    if (after_id.has_value()) {
        if (*after_id <= 0) {
            throw std::invalid_argument("after_id must be positive");
        }
        sql += " AND ue.id > " + std::to_string(*after_id);
    }
}

void maybe_reverse_events(std::vector<UsageEvent> &events, const std::optional<long long> &after_id)
{
    if (after_id.has_value()) {
        std::reverse(events.begin(), events.end());
    }
}

void maybe_reverse_events_with_user(std::vector<UsageEventWithUser> &events, const std::optional<long long> &after_id)
{
    if (after_id.has_value()) {
        std::reverse(events.begin(), events.end());
    }
}

std::string usage_events_base_select(std::string_view prefix)
{
    return "SELECT " + usage_event_columns(prefix);
}

std::string normalize_sql_datetime(std::string_view raw)
{
    const std::string trimmed = trim_ascii(raw);
    if (trimmed.empty()) {
        assert(false && "internal: datetime must not be empty");
        return {};
    }
    return trimmed;
}

std::string normalize_usage_money(std::string_view raw)
{
    std::string trimmed = trim_ascii(raw);
    if (trimmed.empty()) {
        return "0.000000";
    }
    bool seen_dot = false;
    std::string int_part;
    std::string frac_part;
    for (char ch : trimmed) {
        if (ch == '.') {
            if (seen_dot) {
                assert(false && "internal: money format is invalid");
                return "0.000000";
            }
            seen_dot = true;
            continue;
        }
        if (ch < '0' || ch > '9') {
            assert(false && "internal: money format is invalid");
            return "0.000000";
        }
        if (seen_dot) {
            frac_part.push_back(ch);
        } else {
            int_part.push_back(ch);
        }
    }
    if (int_part.empty()) {
        int_part = "0";
    }
    size_t first_non_zero = 0;
    while (first_non_zero + 1 < int_part.size() && int_part[first_non_zero] == '0') {
        ++first_non_zero;
    }
    int_part.erase(0, first_non_zero);
    if (int_part.size() > decimal_integer_digits) {
        assert(false && "internal: money integer digits are too large");
        return "0.000000";
    }
    if (frac_part.size() > decimal_scale) {
        frac_part.resize(decimal_scale);
    }
    while (frac_part.size() < decimal_scale) {
        frac_part.push_back('0');
    }
    return int_part + "." + frac_part;
}

} // namespace

bool usage_model_mismatch(const UsageEvent &event)
{
    const std::string forwarded = optional_string(event.forwarded_model);
    const std::string upstream = optional_string(event.upstream_response_model);
    return !forwarded.empty() && !upstream.empty() && forwarded != upstream;
}

std::string normalize_usage_service_tier(std::string_view raw)
{
    std::string tier = lowercase_ascii(trim_ascii(raw));
    if (tier == "fast") {
        tier = "priority";
    }
    return tier;
}

std::string normalize_usage_service_tier_downgrade_reason(std::string_view raw)
{
    return lowercase_ascii(trim_ascii(raw));
}

std::optional<std::string> normalize_usage_service_tier(const std::optional<std::string> &value)
{
    if (!value.has_value()) {
        return std::nullopt;
    }
    std::string tier = lowercase_ascii(trim_ascii(*value));
    if (tier.empty()) {
        return std::nullopt;
    }
    if (tier == "fast") {
        tier = "priority";
    }
    if (tier.size() > service_tier_max_len) {
        tier.resize(service_tier_max_len);
    }
    return tier;
}

std::optional<std::string> normalize_usage_service_tier_downgrade_reason(const std::optional<std::string> &value)
{
    if (!value.has_value()) {
        return std::nullopt;
    }
    std::string reason = lowercase_ascii(trim_ascii(*value));
    if (reason.empty()) {
        return std::nullopt;
    }
    if (reason != "fast_unavailable" && reason != "capacity") {
        return std::nullopt;
    }
    if (reason.size() > downgrade_reason_max_len) {
        reason.resize(downgrade_reason_max_len);
    }
    return reason;
}

std::optional<UsageEventRecord> row_to_usage_event_record(const MysqlResultRow &row)
{
    if (row.size() < 32 || !row[0].has_value()) {
        return std::nullopt;
    }
    UsageEventRecord event;
    event.id = std::stoll(*row[0]);
    event.request_id = row[1].value_or("");
    event.user_id = std::stoll(row[2].value_or("0"));
    event.token_id = std::stoll(row[3].value_or("0"));
    event.state = row[4].value_or("");
    event.model = row_string(row, 5, model_max_len);
    event.endpoint = row_string(row, 6, endpoint_max_len);
    event.method = row_string(row, 7, method_max_len);
    event.status_code = std::stoi(row[8].value_or("0"));
    event.latency_ms = std::stoi(row[9].value_or("0"));
    event.first_token_latency_ms = std::stoi(row[10].value_or("0"));
    event.error_class = row_string(row, 11, error_class_max_len);
    event.error_message = row_string(row, 12, error_message_max_len);
    event.forwarded_model = row_string(row, 13, model_max_len);
    event.upstream_response_model = row_string(row, 14, model_max_len);
    event.channel_id = row_i64(row, 15);
    event.requested_service_tier = row_string(row, 16, service_tier_max_len);
    event.service_tier = row_string(row, 17, service_tier_max_len);
    event.service_tier_downgrade_reason = row_string(row, 18, downgrade_reason_max_len);
    event.input_tokens = row_i64(row, 19);
    event.cache_read_input_tokens = row_i64(row, 20);
    event.cache_creation_input_tokens = row_i64(row, 21);
    event.cache_creation_1h_input_tokens = row_i64(row, 22);
    event.output_tokens = row_i64(row, 23);
    event.committed_usd = row[24].value_or("0.000000");
    event.price_multiplier = row[25].value_or("1.000000");
    event.price_multiplier_group = row[26].value_or("1.000000");
    event.price_multiplier_payment = row[27].value_or("1.000000");
    event.price_multiplier_group_name = row_string(row, 28, price_group_name_max_len);
    event.is_stream = parse_boolish(row[29].value_or("0"));
    event.request_bytes = std::stoll(row[30].value_or("0"));
    event.response_bytes = std::stoll(row[31].value_or("0"));
    return event;
}

UsageStore::UsageStore(MysqlConnection &conn)
    : conn_(conn)
{
}

long long UsageStore::begin_usage(const UsageBeginInput &input)
{
    assert(input.user_id > 0 && "internal: user_id must be positive");
    if (input.user_id <= 0) {
        return 0;
    }
    assert(input.token_id > 0 && "internal: token_id must be positive");
    if (input.token_id <= 0) {
        return 0;
    }
    const std::string request_id = trim_ascii(input.request_id);
    if (request_id.empty()) {
        assert(false && "internal: request_id must not be empty");
        return 0;
    }
    const std::optional<std::string> model = normalize_model_text(input.model);
    const std::optional<std::string> requested_tier = normalize_usage_service_tier(input.requested_service_tier);
    const std::optional<std::string> service_tier = normalize_usage_service_tier(input.service_tier);
    const std::optional<std::string> downgrade_reason =
        normalize_usage_service_tier_downgrade_reason(input.service_tier_downgrade_reason);
    const std::string committed_usd = normalize_usage_money(input.committed_usd);

    try {
        DbTransaction tr(conn_);
        conn_.exec("INSERT INTO usage_events("
                   "time,request_id,user_id,token_id,state,model,requested_service_tier,service_tier,"
                   "service_tier_downgrade_reason,committed_usd,created_at,updated_at"
                   ") VALUES("
                   "UTC_TIMESTAMP()," +
                   conn_.quote(request_id) + "," + std::to_string(input.user_id) + "," +
                   std::to_string(input.token_id) + "," + conn_.quote(usage_state_pending) + "," +
                   sql_nullable(conn_, model) + "," + sql_nullable(conn_, requested_tier) + "," +
                   sql_nullable(conn_, service_tier) + "," + sql_nullable(conn_, downgrade_reason) + "," +
                   committed_usd + ",UTC_TIMESTAMP(),UTC_TIMESTAMP())");
        const long long id = static_cast<long long>(conn_.last_insert_id());
        tr.commit();
        return id;
    } catch (const std::exception &) {
        const bool duplicate_request_id = is_duplicate_request_id_error(conn_);
        if (duplicate_request_id) {
            const auto existing = get_usage_event_by_request_id(request_id);
            if (existing.has_value()) {
                if (existing->state == usage_state_pending) {
                    return existing->id;
                }
                throw std::runtime_error("request_id already exists in non-pending state");
            }
        }
        throw;
    }
}

bool UsageStore::commit_usage(const UsageCommitInput &input)
{
    assert(input.usage_event_id > 0 && "internal: usage_event_id must be positive");
    if (input.usage_event_id <= 0) {
        return false;
    }
    const std::optional<std::string> requested_tier = normalize_usage_service_tier(input.requested_service_tier);
    const std::optional<std::string> service_tier = normalize_usage_service_tier(input.service_tier);
    const std::optional<std::string> downgrade_reason =
        normalize_usage_service_tier_downgrade_reason(input.service_tier_downgrade_reason);
    const std::string committed_usd = normalize_usage_money(input.committed_usd);
    const std::string price_multiplier = normalize_usage_money(input.price_multiplier);
    const std::string price_multiplier_group = normalize_usage_money(input.price_multiplier_group);
    const std::string price_multiplier_payment = normalize_usage_money(input.price_multiplier_payment);
    const std::optional<std::string> price_group_name =
        nullable_trimmed_limited(input.price_multiplier_group_name, price_group_name_max_len);

    conn_.exec("UPDATE usage_events SET "
               "state=" +
               conn_.quote(usage_state_committed) +
               ","
               "channel_id=" +
               sql_nullable_i64(input.channel_id) +
               ","
               "requested_service_tier=COALESCE(" +
               sql_nullable(conn_, requested_tier) +
               ", requested_service_tier),"
               "service_tier=COALESCE(" +
               sql_nullable(conn_, service_tier) +
               ", service_tier),"
               "service_tier_downgrade_reason=COALESCE(" +
               sql_nullable(conn_, downgrade_reason) +
               ", service_tier_downgrade_reason),"
               "input_tokens=" +
               sql_nullable_i64(input.input_tokens) +
               ","
               "cache_read_input_tokens=" +
               sql_nullable_i64(input.cache_read_input_tokens) +
               ","
               "cache_creation_input_tokens=" +
               sql_nullable_i64(input.cache_creation_input_tokens) +
               ","
               "cache_creation_1h_input_tokens=" +
               sql_nullable_i64(input.cache_creation_1h_input_tokens) +
               ","
               "output_tokens=" +
               sql_nullable_i64(input.output_tokens) +
               ","
               "committed_usd=" +
               committed_usd +
               ","
               "price_multiplier=" +
               price_multiplier +
               ","
               "price_multiplier_group=" +
               price_multiplier_group +
               ","
               "price_multiplier_payment=" +
               price_multiplier_payment +
               ","
               "price_multiplier_group_name=" +
               sql_nullable(conn_, price_group_name) +
               ","
               "updated_at=UTC_TIMESTAMP() "
               "WHERE id=" +
               std::to_string(input.usage_event_id) + " AND state=" + conn_.quote(usage_state_pending));
    return conn_.affected_rows() > 0;
}

bool UsageStore::finalize_usage(const UsageFinalizeInput &input)
{
    if (input.usage_event_id <= 0) {
        throw std::invalid_argument("usage_event_id must be positive");
    }
    const std::optional<std::string> endpoint = nullable_trimmed_limited(input.endpoint, endpoint_max_len);
    const std::optional<std::string> method = nullable_trimmed_limited(input.method, method_max_len);
    const std::optional<std::string> error_class = nullable_trimmed_limited(input.error_class, error_class_max_len);
    const std::optional<std::string> error_message =
        nullable_trimmed_limited(input.error_message, error_message_max_len);
    const std::optional<std::string> forwarded_model = normalize_model_text(input.forwarded_model);
    const std::optional<std::string> upstream_response_model = normalize_model_text(input.upstream_response_model);
    const int latency_ms = normalize_non_negative_int(input.latency_ms);
    int first_token_latency_ms = normalize_non_negative_int(input.first_token_latency_ms);
    if (first_token_latency_ms > latency_ms) {
        first_token_latency_ms = latency_ms;
    }

    conn_.exec("UPDATE usage_events SET "
               "endpoint=COALESCE(" +
               sql_nullable(conn_, endpoint) +
               ", endpoint),"
               "method=COALESCE(" +
               sql_nullable(conn_, method) +
               ", method),"
               "status_code=" +
               std::to_string(normalize_status_code(input.status_code)) +
               ","
               "latency_ms=" +
               std::to_string(latency_ms) +
               ","
               "first_token_latency_ms=" +
               std::to_string(first_token_latency_ms) +
               ","
               "error_class=COALESCE(" +
               sql_nullable(conn_, error_class) +
               ", error_class),"
               "error_message=COALESCE(" +
               sql_nullable(conn_, error_message) +
               ", error_message),"
               "forwarded_model=COALESCE(" +
               sql_nullable(conn_, forwarded_model) +
               ", forwarded_model),"
               "upstream_response_model=COALESCE(" +
               sql_nullable(conn_, upstream_response_model) +
               ", upstream_response_model),"
               "channel_id=COALESCE(" +
               sql_nullable_i64(input.channel_id) +
               ", channel_id),"
               "is_stream=" +
               std::to_string(input.is_stream ? 1 : 0) +
               ","
               "request_bytes=" +
               std::to_string(normalize_non_negative_i64(input.request_bytes)) +
               ","
               "response_bytes=" +
               std::to_string(normalize_non_negative_i64(input.response_bytes)) +
               ","
               "updated_at=UTC_TIMESTAMP() "
               "WHERE id=" +
               std::to_string(input.usage_event_id));
    return conn_.affected_rows() > 0;
}

bool UsageStore::fail_usage(long long usage_event_id, const std::optional<std::string> &error_class,
                            const std::optional<std::string> &error_message)
{
    assert(usage_event_id > 0 && "internal: usage_event_id must be positive");
    if (usage_event_id <= 0) {
        return false;
    }
    const std::optional<std::string> normalized_error_class =
        nullable_trimmed_limited(error_class, error_class_max_len);
    const std::optional<std::string> normalized_error_message =
        nullable_trimmed_limited(error_message, error_message_max_len);
    conn_.exec("UPDATE usage_events SET "
               "state=" +
               conn_.quote(usage_state_failed) +
               ","
               "committed_usd=0.000000,"
               "error_class=COALESCE(" +
               sql_nullable(conn_, normalized_error_class) +
               ", error_class),"
               "error_message=COALESCE(" +
               sql_nullable(conn_, normalized_error_message) +
               ", error_message),"
               "updated_at=UTC_TIMESTAMP() "
               "WHERE id=" +
               std::to_string(usage_event_id) + " AND state=" + conn_.quote(usage_state_pending));
    return conn_.affected_rows() > 0;
}

bool UsageStore::abort_usage(long long usage_event_id)
{
    return fail_usage(usage_event_id, std::optional<std::string>{ "aborted" },
                      std::optional<std::string>{ "request aborted" });
}

UsageExpireResult UsageStore::expire_pending_usage(std::string_view before_utc)
{
    const std::string before = trim_ascii(before_utc);
    if (before.empty()) {
        assert(false && "internal: before_utc must not be empty");
        return {};
    }
    UsageExpireResult result;
    DbTransaction tr(conn_);
    const auto rows = conn_.query_rows("SELECT id FROM usage_events WHERE state=" + conn_.quote(usage_state_pending) +
                                       " AND created_at<" + conn_.quote(before) + " ORDER BY id FOR UPDATE");
    result.ids.reserve(rows.size());
    for (const MysqlResultRow &row : rows) {
        if (row.empty() || !row[0].has_value()) {
            continue;
        }
        result.ids.push_back(std::stoll(*row[0]));
    }
    if (result.ids.empty()) {
        return result;
    }
    std::string in_clause;
    for (size_t i = 0; i < result.ids.size(); ++i) {
        if (i > 0) {
            in_clause += ",";
        }
        in_clause += std::to_string(result.ids[i]);
    }
    conn_.exec("UPDATE usage_events SET state=" + conn_.quote(usage_state_failed) +
               ", committed_usd=0.000000, error_class='expired', error_message='pending usage expired',"
               " updated_at=UTC_TIMESTAMP() WHERE id IN (" +
               in_clause + ") AND state=" + conn_.quote(usage_state_pending));
    const auto verified_rows =
        conn_.query_rows("SELECT id FROM usage_events WHERE id IN (" + in_clause +
                         ") AND state=" + conn_.quote(usage_state_failed) + " AND error_class='expired' ORDER BY id");
    result.expired = static_cast<long long>(verified_rows.size());
    tr.commit();
    return result;
}

std::optional<UsageEventRecord> UsageStore::get_usage_event_by_id(long long usage_event_id)
{
    assert(usage_event_id > 0 && "internal: usage_event_id must be positive");
    if (usage_event_id <= 0) {
        return std::nullopt;
    }
    const auto rows =
        conn_.query_rows(select_usage_event_sql("WHERE id=" + std::to_string(usage_event_id) + " LIMIT 1"));
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_usage_event_record(rows[0]);
}

std::optional<UsageEventRecord> UsageStore::get_usage_event_by_request_id(std::string_view request_id)
{
    const std::string trimmed = trim_ascii(request_id);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    const auto rows = conn_.query_rows(select_usage_event_sql("WHERE request_id=" + conn_.quote(trimmed) + " LIMIT 1"));
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_usage_event_record(rows[0]);
}

bool UsageStore::commit_non_stream_before_response(const UsageCommitInput &commit, const UsageFinalizeInput &finalize,
                                                   UsageCommitAction action)
{
    DbTransaction tr(conn_);
    if (!commit_usage(commit)) {
        return false;
    }
    if (!finalize_usage(finalize)) {
        return false;
    }
    tr.commit();
    if (action) {
        action();
    }
    return true;
}

std::optional<UsageEvent> UsageStore::get_usage_event(long long event_id)
{
    assert(event_id > 0 && "internal: event_id must be positive");
    if (event_id <= 0) {
        return std::nullopt;
    }
    const auto rows = conn_.query_rows(usage_events_base_select("") +
                                       " FROM usage_events WHERE id=" + std::to_string(event_id) + " LIMIT 1");
    auto events = scan_usage_events(rows);
    if (events.empty()) {
        return std::nullopt;
    }
    return events.front();
}

UsageTokenStats UsageStore::get_usage_token_stats_by_user_range(long long user_id, std::string_view since_utc,
                                                                std::string_view until_utc)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    if (user_id <= 0) {
        return {};
    }
    const std::string sql =
        "SELECT COUNT(*) AS requests, "
        "COALESCE(SUM(COALESCE(input_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_read_input_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_creation_input_tokens,0)),0), "
        "COALESCE(SUM(CASE WHEN state='committed' THEN committed_usd ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN first_token_latency_ms ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN 1 ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN latency_ms > 0 THEN latency_ms ELSE 0 END),0) "
        "FROM usage_events WHERE time >= " +
        conn_.quote(normalize_sql_datetime(since_utc)) + " AND time < " +
        conn_.quote(normalize_sql_datetime(until_utc)) + " AND user_id=" + std::to_string(user_id);
    const auto rows = conn_.query_rows(sql);
    return rows.empty() ? UsageTokenStats{} : scan_usage_token_stats_row(rows[0]);
}

UsageTokenStats UsageStore::get_usage_token_stats_by_token_range(long long token_id, std::string_view since_utc,
                                                                 std::string_view until_utc)
{
    assert(token_id > 0 && "internal: token_id must be positive");
    if (token_id <= 0) {
        return {};
    }
    const std::string sql =
        "SELECT COUNT(*) AS requests, "
        "COALESCE(SUM(COALESCE(input_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_read_input_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_creation_input_tokens,0)),0), "
        "COALESCE(SUM(CASE WHEN state='committed' THEN committed_usd ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN first_token_latency_ms ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN 1 ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN latency_ms > 0 THEN latency_ms ELSE 0 END),0) "
        "FROM usage_events WHERE time >= " +
        conn_.quote(normalize_sql_datetime(since_utc)) + " AND time < " +
        conn_.quote(normalize_sql_datetime(until_utc)) + " AND token_id=" + std::to_string(token_id);
    const auto rows = conn_.query_rows(sql);
    return rows.empty() ? UsageTokenStats{} : scan_usage_token_stats_row(rows[0]);
}

UsageTokenStats UsageStore::get_global_usage_stats_range(std::string_view since_utc, std::string_view until_utc)
{
    const std::string sql =
        "SELECT COUNT(*) AS requests, "
        "COALESCE(SUM(COALESCE(input_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_read_input_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_creation_input_tokens,0)),0), "
        "COALESCE(SUM(CASE WHEN state='committed' THEN committed_usd ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN first_token_latency_ms ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN 1 ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN latency_ms > 0 THEN latency_ms ELSE 0 END),0) "
        "FROM usage_events WHERE time >= " +
        conn_.quote(normalize_sql_datetime(since_utc)) + " AND time < " +
        conn_.quote(normalize_sql_datetime(until_utc));
    const auto rows = conn_.query_rows(sql);
    return rows.empty() ? UsageTokenStats{} : scan_usage_token_stats_row(rows[0]);
}

std::vector<UsageEvent> UsageStore::list_usage_events_by_token(long long token_id, int limit,
                                                               const std::optional<long long> &before_id)
{
    return list_usage_events_by_token_range(token_id, "1000-01-01 00:00:00", "9999-12-31 23:59:59", limit, before_id,
                                            std::nullopt);
}

std::vector<UsageEvent> UsageStore::list_usage_events_by_token_range(long long token_id, std::string_view since_utc,
                                                                     std::string_view until_utc, int limit,
                                                                     const std::optional<long long> &before_id,
                                                                     const std::optional<long long> &after_id)
{
    assert(token_id > 0 && "internal: token_id must be positive");
    if (token_id <= 0) {
        return {};
    }
    limit = normalize_usage_event_limit(limit, false);
    std::string sql = usage_events_base_select("ue.") +
                      " FROM usage_events ue WHERE ue.token_id=" + std::to_string(token_id) +
                      " AND ue.time >= " + conn_.quote(normalize_sql_datetime(since_utc)) + " AND ue.time < " +
                      conn_.quote(normalize_sql_datetime(until_utc));
    append_usage_event_cursor(sql, before_id, after_id);
    if (after_id.has_value()) {
        sql += " ORDER BY ue.id ASC";
    } else {
        sql += " ORDER BY ue.id DESC";
    }
    sql += " LIMIT " + std::to_string(limit);
    auto events = scan_usage_events(conn_.query_rows(sql));
    maybe_reverse_events(events, after_id);
    return events;
}

std::vector<UsageEvent> UsageStore::list_usage_events_by_user_filtered(long long user_id, int limit,
                                                                       const std::optional<long long> &before_id,
                                                                       const std::optional<long long> &after_id,
                                                                       const UsageEventsIndexFlags &index_flags,
                                                                       const UsageEventsFilters &filters)
{
    return list_usage_events_by_user_range_filtered(user_id, "1000-01-01 00:00:00", "9999-12-31 23:59:59", limit,
                                                    before_id, after_id, index_flags, filters);
}

std::vector<UsageEvent> UsageStore::list_usage_events_by_user_range_filtered(
    long long user_id, std::string_view since_utc, std::string_view until_utc, int limit,
    const std::optional<long long> &before_id, const std::optional<long long> &after_id,
    const UsageEventsIndexFlags &index_flags, const UsageEventsFilters &filters)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    if (user_id <= 0) {
        return {};
    }
    limit = normalize_usage_event_limit(limit, false);
    UsageEventsFilters normalized = filters;
    normalized.key = sanitize_query_input(normalized.key);
    normalized.model = sanitize_query_input(normalized.model);
    const UsageEventQueryParts parts = build_usage_event_filter_sql(conn_, index_flags, normalized, false);
    std::string sql = usage_events_base_select("ue.") + " FROM usage_events ue" + parts.joins +
                      " WHERE ue.user_id=" + std::to_string(user_id) +
                      " AND ue.time >= " + conn_.quote(normalize_sql_datetime(since_utc)) + " AND ue.time < " +
                      conn_.quote(normalize_sql_datetime(until_utc)) + parts.where;
    append_usage_event_cursor(sql, before_id, after_id);
    if (after_id.has_value()) {
        sql += " ORDER BY ue.id ASC";
    } else {
        sql += " ORDER BY ue.id DESC";
    }
    sql += " LIMIT " + std::to_string(limit);
    auto events = scan_usage_events(conn_.query_rows(sql));
    maybe_reverse_events(events, after_id);
    return events;
}

std::vector<UsageEventWithUser> UsageStore::list_usage_events_with_user_range_filtered(
    std::string_view since_utc, std::string_view until_utc, int limit, const std::optional<long long> &before_id,
    const std::optional<long long> &after_id, const UsageEventsIndexFlags &index_flags,
    const UsageEventsFilters &filters)
{
    limit = normalize_usage_event_limit(limit, true);
    UsageEventsFilters normalized = filters;
    normalized.user = sanitize_query_input(normalized.user);
    normalized.key = sanitize_query_input(normalized.key);
    normalized.channel = sanitize_query_input(normalized.channel);
    normalized.model = sanitize_query_input(normalized.model);
    const UsageEventQueryParts parts = build_usage_event_filter_sql(conn_, index_flags, normalized, true);
    std::string sql = "SELECT " + usage_event_columns("ue.") +
                      ", COALESCE(u.email,'') FROM usage_events ue "
                      "LEFT JOIN users u ON u.id=ue.user_id" +
                      parts.joins + " WHERE ue.time >= " + conn_.quote(normalize_sql_datetime(since_utc)) +
                      " AND ue.time < " + conn_.quote(normalize_sql_datetime(until_utc)) + parts.where;
    append_usage_event_cursor(sql, before_id, after_id);
    if (after_id.has_value()) {
        sql += " ORDER BY ue.id ASC";
    } else {
        sql += " ORDER BY ue.id DESC";
    }
    sql += " LIMIT " + std::to_string(limit);
    auto events = scan_usage_events_with_user(conn_.query_rows(sql));
    maybe_reverse_events_with_user(events, after_id);
    return events;
}

std::vector<UsageTimeSeriesPoint> UsageStore::get_user_usage_time_series_range(long long user_id,
                                                                               std::string_view since_utc,
                                                                               std::string_view until_utc,
                                                                               std::string_view granularity)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    if (user_id <= 0) {
        return {};
    }
    const std::string g = normalize_granularity(granularity);
    const std::string bucket = usage_bucket_expr(g, "time");
    const std::string sql =
        "SELECT " + bucket +
        " AS bucket, COUNT(*) AS requests, "
        "COALESCE(SUM(COALESCE(input_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_read_input_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_creation_input_tokens,0)),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN first_token_latency_ms ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN 1 ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN latency_ms > 0 THEN latency_ms ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN state='committed' THEN committed_usd ELSE 0 END),0) "
        "FROM usage_events WHERE user_id=" +
        std::to_string(user_id) + " AND time >= " + conn_.quote(normalize_sql_datetime(since_utc)) + " AND time < " +
        conn_.quote(normalize_sql_datetime(until_utc)) + " GROUP BY bucket ORDER BY bucket ASC";
    return scan_usage_time_series(conn_.query_rows(sql));
}

std::vector<UsageTimeSeriesPoint> UsageStore::get_token_usage_time_series_range(long long token_id,
                                                                                std::string_view since_utc,
                                                                                std::string_view until_utc,
                                                                                std::string_view granularity)
{
    assert(token_id > 0 && "internal: token_id must be positive");
    if (token_id <= 0) {
        return {};
    }
    const std::string g = normalize_granularity(granularity);
    const std::string bucket = usage_bucket_expr(g, "time");
    const std::string sql =
        "SELECT " + bucket +
        " AS bucket, COUNT(*) AS requests, "
        "COALESCE(SUM(COALESCE(input_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_read_input_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_creation_input_tokens,0)),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN first_token_latency_ms ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN 1 ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN latency_ms > 0 THEN latency_ms ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN state='committed' THEN committed_usd ELSE 0 END),0) "
        "FROM usage_events WHERE token_id=" +
        std::to_string(token_id) + " AND time >= " + conn_.quote(normalize_sql_datetime(since_utc)) + " AND time < " +
        conn_.quote(normalize_sql_datetime(until_utc)) + " GROUP BY bucket ORDER BY bucket ASC";
    return scan_usage_time_series(conn_.query_rows(sql));
}

std::vector<UsageTimeSeriesPoint> UsageStore::get_global_usage_time_series_range(std::string_view since_utc,
                                                                                 std::string_view until_utc,
                                                                                 std::string_view granularity)
{
    const std::string g = normalize_granularity(granularity);
    const std::string bucket = usage_bucket_expr(g, "time");
    const std::string sql =
        "SELECT " + bucket +
        " AS bucket, COUNT(*) AS requests, "
        "COALESCE(SUM(COALESCE(input_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_read_input_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_creation_input_tokens,0)),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN first_token_latency_ms ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN 1 ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN latency_ms > 0 THEN latency_ms ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN state='committed' THEN committed_usd ELSE 0 END),0) "
        "FROM usage_events WHERE time >= " +
        conn_.quote(normalize_sql_datetime(since_utc)) + " AND time < " +
        conn_.quote(normalize_sql_datetime(until_utc)) + " GROUP BY bucket ORDER BY bucket ASC";
    return scan_usage_time_series(conn_.query_rows(sql));
}

std::vector<UsageTopUser> UsageStore::list_usage_top_users(std::string_view since_utc, std::string_view until_utc,
                                                           int limit)
{
    limit = normalize_usage_event_limit(limit, true);
    const std::string sql = "SELECT ue.user_id, COALESCE(u.email,''), COALESCE(u.role,''), COALESCE(u.status,1), "
                            "COALESCE(SUM(ue.committed_usd),0) "
                            "FROM usage_events ue LEFT JOIN users u ON u.id=ue.user_id "
                            "WHERE ue.time >= " +
                            conn_.quote(normalize_sql_datetime(since_utc)) + " AND ue.time < " +
                            conn_.quote(normalize_sql_datetime(until_utc)) +
                            " AND ue.state='committed' "
                            "GROUP BY ue.user_id, u.email, u.role, u.status "
                            "ORDER BY SUM(ue.committed_usd) DESC LIMIT " +
                            std::to_string(limit);
    const auto rows = conn_.query_rows(sql);
    std::vector<UsageTopUser> out;
    out.reserve(rows.size());
    for (const MysqlResultRow &row : rows) {
        if (row.size() < 5 || !row[0].has_value()) {
            continue;
        }
        UsageTopUser item;
        item.user_id = parse_i64_or_zero(row[0]);
        item.email = row[1].value_or("");
        item.role = row[2].value_or("");
        item.status = parse_int_or_zero(row[3]);
        item.committed_usd = trim_ascii(row[4].value_or("0"));
        if (item.committed_usd.empty()) {
            item.committed_usd = "0";
        }
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<UsageUserSuggest> UsageStore::suggest_users(std::string_view q, int limit)
{
    const std::string query = sanitize_query_input(q);
    if (query.empty()) {
        return {};
    }
    limit = normalize_suggest_limit(limit);
    const std::string pattern = build_like_pattern(query);
    const std::string sql = "SELECT id, COALESCE(email,''), COALESCE(username,'') FROM users "
                            "WHERE email LIKE " +
                            conn_.quote(pattern) + " OR username LIKE " + conn_.quote(pattern) +
                            " ORDER BY id DESC LIMIT " + std::to_string(limit);
    const auto rows = conn_.query_rows(sql);
    std::vector<UsageUserSuggest> out;
    out.reserve(rows.size());
    for (const MysqlResultRow &row : rows) {
        if (row.size() < 3 || !row[0].has_value()) {
            continue;
        }
        UsageUserSuggest item;
        item.id = parse_i64_or_zero(row[0]);
        item.email = row[1].value_or("");
        item.username = row[2].value_or("");
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<UsageChannelSuggest> UsageStore::suggest_usage_channels(std::string_view since_utc,
                                                                    std::string_view until_utc, std::string_view q,
                                                                    int limit)
{
    const std::string query = sanitize_query_input(q);
    if (query.empty()) {
        return {};
    }
    limit = normalize_suggest_limit(limit);
    const int scan_limit = std::max(limit * 8, 32);
    const std::string sql = "SELECT uc.id, uc.name, uc.type FROM usage_events ue "
                            "JOIN channels uc ON uc.id=ue.channel_id "
                            "WHERE ue.time >= " +
                            conn_.quote(normalize_sql_datetime(since_utc)) + " AND ue.time < " +
                            conn_.quote(normalize_sql_datetime(until_utc)) +
                            " AND ue.state='committed' AND ue.channel_id > 0 "
                            " AND uc.name LIKE " +
                            conn_.quote(build_like_pattern(query)) + " ORDER BY ue.time DESC, ue.id DESC LIMIT " +
                            std::to_string(scan_limit);
    const auto rows = conn_.query_rows(sql);
    std::vector<UsageChannelSuggest> out;
    out.reserve(limit);
    std::unordered_set<long long> seen;
    for (const MysqlResultRow &row : rows) {
        if (row.size() < 3 || !row[0].has_value()) {
            continue;
        }
        const long long id = parse_i64_or_zero(row[0]);
        if (id <= 0 || !seen.insert(id).second) {
            continue;
        }
        UsageChannelSuggest item;
        item.id = id;
        item.name = row[1].value_or("");
        item.type = row[2].value_or("");
        out.push_back(std::move(item));
        if (static_cast<int>(out.size()) >= limit) {
            break;
        }
    }
    return out;
}

std::vector<std::string> UsageStore::suggest_usage_models(std::string_view since_utc, std::string_view until_utc,
                                                          std::string_view q, int limit)
{
    const std::string query = sanitize_query_input(q);
    if (query.empty()) {
        return {};
    }
    limit = normalize_suggest_limit(limit);
    const int scan_limit = std::max(limit * 8, 32);
    const std::string sql = "SELECT ue.model FROM usage_events ue FORCE INDEX (idx_usage_events_model_time_id) "
                            "WHERE ue.time >= " +
                            conn_.quote(normalize_sql_datetime(since_utc)) + " AND ue.time < " +
                            conn_.quote(normalize_sql_datetime(until_utc)) +
                            " AND ue.state='committed' AND ue.model IS NOT NULL AND ue.model<>'' "
                            " AND ue.model LIKE " +
                            conn_.quote(build_like_pattern(query)) + " ORDER BY ue.time DESC, ue.id DESC LIMIT " +
                            std::to_string(scan_limit);
    const auto rows = conn_.query_rows(sql);
    std::vector<std::string> out;
    out.reserve(limit);
    std::unordered_set<std::string> seen;
    for (const MysqlResultRow &row : rows) {
        if (row.empty() || !row[0].has_value()) {
            continue;
        }
        const std::string model = trim_ascii(*row[0]);
        if (model.empty() || !seen.insert(model).second) {
            continue;
        }
        out.push_back(model);
        if (static_cast<int>(out.size()) >= limit) {
            break;
        }
    }
    return out;
}

} // namespace revlm
