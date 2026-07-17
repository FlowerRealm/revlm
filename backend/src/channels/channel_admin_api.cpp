#include "channels/channels.hpp"
#include "auth/session.hpp"
#include "server/http_server.hpp"
#include "users/users.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "store/database.hpp"
#include "request/request.hpp"
#include "util/http_query.hpp"
#include "util/json_convert.hpp"
#include "util/user_input.hpp"
#include "util/json_util.hpp"

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <iomanip>
#include <ios>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <time.h>
#include <unordered_map>
#include <utility>
#include <vector>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

struct ParsedRequest {
    std::string_view method;
    std::string_view path;
    std::string_view target;
};

struct RootAuth {
    bool ok = false;
    bool clear_cookie = false;
    std::string failure;
    long long actor_user_id = 0;
};

HttpResponse api_json_response(boost::json::value body, std::vector<Header> headers = {})
{
    return http_response(200, "OK", std::move(body), std::move(headers));
}

RootAuth authenticate_root_admin(std::string_view raw_request)
{
    RootAuth out;
    const WebSessionAuth auth = authenticate_root_web_session(raw_request);
    out.clear_cookie = auth.clear_cookie;
    if (!auth.ok) {
        out.failure = auth.failure_message;
        return out;
    }
    if (auth.user.has_value()) {
        out.actor_user_id = auth.user->id;
    }
    out.ok = true;
    return out;
}

struct ChannelPageWindow {
    std::string start;
    std::string end;
    bool all_time = false;
};

struct ChannelTimeSeriesRequest {
    long long channel_id = 0;
    std::string start;
    std::string end;
    bool all_time = false;
    std::string granularity = "hour";
};

struct ChannelUsageMetrics {
    long long requests = 0;
    long long tokens = 0;
    double usd = 0.0;
    double cache_ratio = 0.0;
    double avg_first_token_latency_ms = 0.0;
    double tokens_per_second = 0.0;
};

struct ChannelRuntimeSnapshot {
    bool available = true;
    std::optional<int> fail_score;
};

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
            if (static_cast<unsigned char>(ch) < 0x20) {
                out += "\\u00";
                constexpr char hex[] = "0123456789abcdef";
                out.push_back(hex[(ch >> 4) & 0xf]);
                out.push_back(hex[ch & 0xf]);
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    return out;
}

boost::json::value api_success()
{
    boost::json::object body;
    body["success"] = true;
    body["message"] = "";
    return body;
}

boost::json::value api_success(boost::json::value data)
{
    boost::json::object body;
    body["success"] = true;
    body["message"] = "";
    body["data"] = std::move(data);
    return body;
}

boost::json::value api_failure(std::string_view message)
{
    boost::json::object body;
    body["success"] = false;
    body["message"] = message;
    return body;
}

HttpResponse admin_auth_failure(std::string_view request_id, std::string_view message, bool clear_cookie,
                                std::string_view raw_request)
{
    std::vector<Header> headers{ { "X-Request-Id", std::string{ request_id } } };
    if (clear_cookie) {
        headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
    }
    return api_json_response(api_failure(message), std::move(headers));
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

std::string optional_json_int(const std::optional<int> &value)
{
    if (!value.has_value()) {
        return "null";
    }
    return std::to_string(*value);
}

std::string decimal_string(double value, int precision)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::string trim_decimal_zeros(std::string value)
{
    const size_t dot = value.find('.');
    if (dot == std::string::npos) {
        return value;
    }
    while (!value.empty() && value.back() == '0') {
        value.pop_back();
    }
    if (!value.empty() && value.back() == '.') {
        value.pop_back();
    }
    if (value.empty()) {
        return "0";
    }
    return value;
}

std::string json_string_from_double(double value, int precision)
{
    return "\"" + trim_decimal_zeros(decimal_string(value, precision)) + "\"";
}

std::string channel_usage_json(const ChannelUsageMetrics &usage)
{
    return "{\"usd\":" + json_string_from_double(usage.usd, 6) + ",\"tokens\":" + std::to_string(usage.tokens) +
           ",\"cache_ratio\":" + json_string_from_double(usage.cache_ratio * 100.0, 1) +
           ",\"avg_first_token_latency\":" + json_string_from_double(usage.avg_first_token_latency_ms, 1) +
           ",\"tokens_per_second\":" + json_string_from_double(usage.tokens_per_second, 2) + "}";
}

std::string channel_runtime_json(const ChannelRuntimeSnapshot &runtime)
{
    return "{\"available\":" + std::string(runtime.available ? "true" : "false") +
           ",\"fail_score\":" + optional_json_int(runtime.fail_score) + "}";
}

std::string channel_json(const Channel &channel, const std::optional<bool> &in_use = std::nullopt,
                         const std::optional<ChannelUsageMetrics> &usage = std::nullopt,
                         const std::optional<ChannelRuntimeSnapshot> &runtime = std::nullopt)
{
    boost::json::object body = to_json(channel);
    if (in_use.has_value()) {
        body["in_use"] = *in_use;
        boost::system::error_code ec;
        body["usage"] = boost::json::parse(channel_usage_json(usage.value_or(ChannelUsageMetrics{})), ec);
        body["runtime"] = boost::json::parse(channel_runtime_json(runtime.value_or(ChannelRuntimeSnapshot{})), ec);
    }
    return boost::json::serialize(body);
}
} // namespace
std::string current_utc_mysql_datetime()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return buffer;
}

bool parse_channel_page_window(std::string_view target, ChannelPageWindow &window, std::string &error)
{
    const auto params = parse_query_params(target);
    window.start = trim_ascii(query_param_value(params, "start"));
    window.end = trim_ascii(query_param_value(params, "end"));
    const std::string all_time = trim_ascii(query_param_value(params, "all_time"));
    if (!all_time.empty()) {
        const auto flag = parse_bool_value(all_time);
        if (!flag.has_value()) {
            error = "all_time 参数无效";
            return false;
        }
        window.all_time = *flag;
    }
    if (!window.all_time && window.start.empty() && window.end.empty()) {
        const std::string now = current_utc_mysql_datetime();
        window.end = now;
        const auto now_secs = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        window.start = mysql_datetime_from_unix(static_cast<long long>(now_secs) - 7 * 24 * 3600);
    }
    if (window.all_time) {
        window.start.clear();
        window.end.clear();
    } else if (window.start.empty()) {
        error = "start 不能为空";
        return false;
    } else if (window.end.empty()) {
        error = "end 不能为空";
        return false;
    }
    return true;
}

bool parse_channel_time_series_request(const ParsedRequest &parsed, ChannelTimeSeriesRequest &req, std::string &error)
{
    constexpr std::string_view prefix = "/api/channel/";
    constexpr std::string_view suffix = "/timeseries";
    if (!parsed.path.starts_with(prefix) || !parsed.path.ends_with(suffix) ||
        parsed.path.size() <= prefix.size() + suffix.size()) {
        error = "渠道不存在";
        return false;
    }
    const std::string_view id_view =
        parsed.path.substr(prefix.size(), parsed.path.size() - prefix.size() - suffix.size());
    const auto channel_id = parse_long_long(id_view);
    if (!channel_id.has_value() || *channel_id <= 0) {
        error = "渠道 ID 无效";
        return false;
    }
    req.channel_id = *channel_id;

    ChannelPageWindow window;
    if (!parse_channel_page_window(parsed.target, window, error)) {
        return false;
    }
    req.start = window.start;
    req.end = window.end;
    req.all_time = window.all_time;
    const auto params = parse_query_params(parsed.target);
    const std::string granularity = lowercase_ascii(trim_ascii(query_param_value(params, "granularity")));
    if (!granularity.empty()) {
        if (granularity != "hour" && granularity != "day") {
            error = "granularity 参数无效";
            return false;
        }
        req.granularity = granularity;
    }
    if (req.all_time) {
        req.granularity = req.granularity.empty() ? "day" : req.granularity;
    }
    return true;
}

std::optional<Channel> find_channel(ChannelStore &store, long long channel_id)
{
    for (const Channel &channel : store.list_channels()) {
        if (channel.id == channel_id) {
            return channel;
        }
    }
    return std::nullopt;
}

bool channel_is_anthropic(int type)
{
    return type == 4;
}

double parse_double_value(const std::optional<std::string> &value)
{
    if (!value.has_value()) {
        return 0.0;
    }
    try {
        return std::stod(*value);
    } catch (const std::exception &) {
        return 0.0;
    }
}

long long parse_long_long_value(const std::optional<std::string> &value)
{
    if (!value.has_value()) {
        return 0;
    }
    try {
        return std::stoll(*value);
    } catch (const std::exception &) {
        return 0;
    }
}

double compute_tokens_per_second(long long output_tokens, long long decode_latency_ms)
{
    if (output_tokens <= 0 || decode_latency_ms <= 0) {
        return 0.0;
    }
    return static_cast<double>(output_tokens) * 1000.0 / static_cast<double>(decode_latency_ms);
}

ChannelUsageMetrics channel_usage_metrics_from_row(const SqlResultRow &row)
{
    ChannelUsageMetrics metrics;
    metrics.requests = row.size() > 0 ? parse_long_long_value(row[0]) : 0;
    const long long total_tokens = row.size() > 1 ? parse_long_long_value(row[1]) : 0;
    const long long cached_tokens = row.size() > 2 ? parse_long_long_value(row[2]) : 0;
    const long long first_token_samples = row.size() > 3 ? parse_long_long_value(row[3]) : 0;
    const long long first_token_latency_sum = row.size() > 4 ? parse_long_long_value(row[4]) : 0;
    const long long output_tokens = row.size() > 5 ? parse_long_long_value(row[5]) : 0;
    const long long decode_latency_sum = row.size() > 6 ? parse_long_long_value(row[6]) : 0;

    metrics.tokens = total_tokens;
    if (total_tokens > 0 && cached_tokens > 0) {
        metrics.cache_ratio = static_cast<double>(cached_tokens) / static_cast<double>(total_tokens);
    }
    if (first_token_samples > 0 && first_token_latency_sum > 0) {
        metrics.avg_first_token_latency_ms =
            static_cast<double>(first_token_latency_sum) / static_cast<double>(first_token_samples);
    }
    metrics.tokens_per_second = compute_tokens_per_second(output_tokens, decode_latency_sum);
    return metrics;
}

ChannelRuntimeSnapshot runtime_snapshot_for_channel(const Channel &channel)
{
    ChannelRuntimeSnapshot runtime;
    runtime.available = channel.status && !trim_ascii(channel.api_key).empty();
    return runtime;
}

std::string channels_page_json(const ChannelPageWindow &window)
{
    odb::database &db = database();
    ChannelStore &store = ChannelStore::instance();
    const auto channels = store.list_channels();
    ChannelGroupStore &group_store = ChannelGroupStore::instance();
    std::unordered_map<long long, bool> used_channels;
    for (const ChannelGroup &group : group_store.list_channel_groups()) {
        for (const Channel &member : group.channels) {
            used_channels[member.id] = true;
        }
    }

    std::string overview_filter;
    if (!window.all_time) {
        overview_filter = " WHERE time>=" + sql_quote(db, window.start) + " AND time<=" + sql_quote(db, window.end);
    }
    const auto overview_rows = sql_query_rows(
        db,
        "SELECT COUNT(*), "
        "COALESCE(SUM(COALESCE(input_tokens,0)+COALESCE(output_tokens,0)),0), "
        "COALESCE(SUM(COALESCE(cache_read_tokens,0)+COALESCE(cache_creation_5m_tokens,0)+COALESCE(cache_creation_1h_tokens,0)),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN 1 ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN first_token_latency_ms ELSE 0 END),0), "
        "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
        "COALESCE(SUM(CASE WHEN latency_ms > first_token_latency_ms THEN latency_ms-first_token_latency_ms ELSE 0 END),0) "
        "FROM requests" +
            overview_filter);
    ChannelUsageMetrics overview = !overview_rows.empty() ? channel_usage_metrics_from_row(overview_rows[0]) :
                                                            ChannelUsageMetrics{};
    {
        const auto price_rows = sql_query_rows(
            db, "SELECT model,input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
                "output_tokens,tier_multiplier,channel_multiplier FROM requests" +
                    overview_filter);
        double used = 0.0;
        for (const SqlResultRow &row : price_rows) {
            Request req;
            req.model_name = trim_ascii(row.size() > 0 ? row[0].value_or("") : "");
            req.input_tokens = static_cast<int>(parse_long_long_value(row.size() > 1 ? row[1] : std::nullopt));
            req.cache_read_tokens = static_cast<int>(parse_long_long_value(row.size() > 2 ? row[2] : std::nullopt));
            req.cache_creation_5m_tokens =
                static_cast<int>(parse_long_long_value(row.size() > 3 ? row[3] : std::nullopt));
            req.cache_creation_1h_tokens =
                static_cast<int>(parse_long_long_value(row.size() > 4 ? row[4] : std::nullopt));
            req.output_tokens = static_cast<int>(parse_long_long_value(row.size() > 5 ? row[5] : std::nullopt));
            const std::string tier_raw = trim_ascii(row.size() > 6 ? row[6].value_or("") : "");
            req.tier_multiplier = tier_raw.empty() ? 1.0 : parse_double_value(row[6]);
            const std::string channel_raw = trim_ascii(row.size() > 7 ? row[7].value_or("") : "");
            req.channel_multiplier = channel_raw.empty() ? 1.0 : parse_double_value(row[7]);
            hydrate_request_model(req);
            used += req.solve_price();
        }
        overview.usd = used;
    }

    std::string json = "{\"admin_time_zone\":\"Asia/Shanghai\",\"start\":";
    json += window.all_time ? "null" : ("\"" + json_escape(window.start) + "\"");
    json += ",\"end\":";
    json += window.all_time ? "null" : ("\"" + json_escape(window.end) + "\"");
    json += ",\"overview\":{\"requests\":" + std::to_string(overview.requests) +
            ",\"tokens\":" + std::to_string(overview.tokens) + ",\"usd\":" + json_string_from_double(overview.usd, 6) +
            ",\"cache_ratio\":" + json_string_from_double(overview.cache_ratio * 100.0, 1) +
            ",\"avg_first_token_latency\":" + json_string_from_double(overview.avg_first_token_latency_ms, 1) +
            ",\"tokens_per_second\":" + json_string_from_double(overview.tokens_per_second, 2) + "},";
    json += "\"channels\":[";
    for (size_t i = 0; i < channels.size(); ++i) {
        if (i > 0) {
            json += ",";
        }
        std::string usage_filter = " WHERE channel_id=" + std::to_string(channels[i].id);
        if (!window.all_time) {
            usage_filter += " AND time>=" + sql_quote(db, window.start) + " AND time<=" + sql_quote(db, window.end);
        }
        const auto usage_rows = sql_query_rows(
            db,
            "SELECT COUNT(*), "
            "COALESCE(SUM(COALESCE(input_tokens,0)+COALESCE(output_tokens,0)),0), "
            "COALESCE(SUM(COALESCE(cache_read_tokens,0)+COALESCE(cache_creation_5m_tokens,0)+COALESCE(cache_creation_1h_tokens,0)),0), "
            "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN 1 ELSE 0 END),0), "
            "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN first_token_latency_ms ELSE 0 END),0), "
            "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
            "COALESCE(SUM(CASE WHEN latency_ms > first_token_latency_ms THEN latency_ms-first_token_latency_ms ELSE 0 END),0) "
            "FROM requests" +
                usage_filter);
        ChannelUsageMetrics usage = !usage_rows.empty() ? channel_usage_metrics_from_row(usage_rows[0]) :
                                                          ChannelUsageMetrics{};
        {
            const auto price_rows = sql_query_rows(
                db, "SELECT model,input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
                    "output_tokens,tier_multiplier,channel_multiplier FROM requests" +
                        usage_filter);
            double used = 0.0;
            for (const SqlResultRow &row : price_rows) {
                Request req;
                req.model_name = trim_ascii(row.size() > 0 ? row[0].value_or("") : "");
                req.input_tokens = static_cast<int>(parse_long_long_value(row.size() > 1 ? row[1] : std::nullopt));
                req.cache_read_tokens = static_cast<int>(parse_long_long_value(row.size() > 2 ? row[2] : std::nullopt));
                req.cache_creation_5m_tokens =
                    static_cast<int>(parse_long_long_value(row.size() > 3 ? row[3] : std::nullopt));
                req.cache_creation_1h_tokens =
                    static_cast<int>(parse_long_long_value(row.size() > 4 ? row[4] : std::nullopt));
                req.output_tokens = static_cast<int>(parse_long_long_value(row.size() > 5 ? row[5] : std::nullopt));
                const std::string tier_raw = trim_ascii(row.size() > 6 ? row[6].value_or("") : "");
                req.tier_multiplier = tier_raw.empty() ? 1.0 : parse_double_value(row[6]);
                const std::string channel_raw = trim_ascii(row.size() > 7 ? row[7].value_or("") : "");
                req.channel_multiplier = channel_raw.empty() ? 1.0 : parse_double_value(row[7]);
                hydrate_request_model(req);
                used += req.solve_price();
            }
            usage.usd = used;
        }
        const ChannelRuntimeSnapshot runtime = runtime_snapshot_for_channel(channels[i]);
        json += channel_json(channels[i], used_channels.contains(channels[i].id), usage, runtime);
    }
    json += "]}";
    return json;
}

std::string channel_time_series_json(const ChannelTimeSeriesRequest &req)
{
    odb::database &db = database();
    ChannelStore &store = ChannelStore::instance();
    const auto channel = find_channel(store, req.channel_id);
    if (!channel.has_value()) {
        throw std::invalid_argument("渠道不存在");
    }

    std::string bucket_expr;
    std::string time_filter = " WHERE channel_id=" + std::to_string(req.channel_id);
    std::string start_value = req.start;
    std::string end_value = req.end;

    if (req.granularity == "day") {
        bucket_expr = "DATE_FORMAT(time, '%Y-%m-%d 00:00:00')";
    } else {
        bucket_expr = "DATE_FORMAT(time, '%Y-%m-%d %H:00:00')";
    }

    if (!req.all_time) {
        time_filter += " AND time>=" + sql_quote(db, start_value) + " AND time<=" + sql_quote(db, end_value);
    } else {
        const auto min_max = sql_query_rows(db, "SELECT MIN(time), MAX(time) FROM requests WHERE channel_id=" +
                                                    std::to_string(req.channel_id));
        if (!min_max.empty() && min_max[0].size() >= 2) {
            start_value = min_max[0][0].value_or("");
            end_value = min_max[0][1].value_or("");
        }
    }

    const auto rows = sql_query_rows(
        db,
        "SELECT " + bucket_expr +
            " AS bucket, "
            "COALESCE(SUM(COALESCE(input_tokens,0)+COALESCE(output_tokens,0)),0), "
            "COALESCE(SUM(COALESCE(cache_read_tokens,0)+COALESCE(cache_creation_5m_tokens,0)+COALESCE(cache_creation_1h_tokens,0)),0), "
            "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN 1 ELSE 0 END),0), "
            "COALESCE(SUM(CASE WHEN first_token_latency_ms > 0 THEN first_token_latency_ms ELSE 0 END),0), "
            "COALESCE(SUM(COALESCE(output_tokens,0)),0), "
            "COALESCE(SUM(CASE WHEN latency_ms > first_token_latency_ms THEN latency_ms-first_token_latency_ms ELSE 0 END),0) "
            "FROM requests" +
            time_filter + " GROUP BY bucket ORDER BY bucket ASC");

    const auto price_rows = sql_query_rows(
        db, "SELECT " + bucket_expr +
                " AS bucket, model,input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
                "output_tokens,tier_multiplier,channel_multiplier FROM requests" +
                time_filter);
    std::unordered_map<std::string, double> used_by_bucket;
    for (const SqlResultRow &row : price_rows) {
        if (row.empty() || !row[0].has_value()) {
            continue;
        }
        Request req;
        req.model_name = trim_ascii(row.size() > 1 ? row[1].value_or("") : "");
        req.input_tokens = static_cast<int>(parse_long_long_value(row.size() > 2 ? row[2] : std::nullopt));
        req.cache_read_tokens = static_cast<int>(parse_long_long_value(row.size() > 3 ? row[3] : std::nullopt));
        req.cache_creation_5m_tokens = static_cast<int>(parse_long_long_value(row.size() > 4 ? row[4] : std::nullopt));
        req.cache_creation_1h_tokens = static_cast<int>(parse_long_long_value(row.size() > 5 ? row[5] : std::nullopt));
        req.output_tokens = static_cast<int>(parse_long_long_value(row.size() > 6 ? row[6] : std::nullopt));
        const std::string tier_raw = trim_ascii(row.size() > 7 ? row[7].value_or("") : "");
        req.tier_multiplier = tier_raw.empty() ? 1.0 : parse_double_value(row[7]);
        const std::string channel_raw = trim_ascii(row.size() > 8 ? row[8].value_or("") : "");
        req.channel_multiplier = channel_raw.empty() ? 1.0 : parse_double_value(row[8]);
        hydrate_request_model(req);
        used_by_bucket[*row[0]] += req.solve_price();
    }

    std::string json = "{\"admin_time_zone\":\"Asia/Shanghai\",\"channel_id\":" + std::to_string(req.channel_id) +
                       ",\"start\":\"" + json_escape(start_value) + "\",\"end\":\"" + json_escape(end_value) +
                       "\",\"granularity\":\"" + json_escape(req.granularity) + "\",\"points\":[";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) {
            json += ",";
        }
        const auto &row = rows[i];
        const std::string bucket = row.size() > 0 && row[0].has_value() ? *row[0] : "";
        const double usd = used_by_bucket[bucket];
        const long long tokens = row.size() > 1 ? parse_long_long_value(row[1]) : 0;
        const long long cached_tokens = row.size() > 2 ? parse_long_long_value(row[2]) : 0;
        const long long first_token_samples = row.size() > 3 ? parse_long_long_value(row[3]) : 0;
        const long long first_token_latency_sum = row.size() > 4 ? parse_long_long_value(row[4]) : 0;
        const long long output_tokens = row.size() > 5 ? parse_long_long_value(row[5]) : 0;
        const long long decode_latency_sum = row.size() > 6 ? parse_long_long_value(row[6]) : 0;
        const double cache_ratio =
            tokens > 0 && cached_tokens > 0 ? static_cast<double>(cached_tokens) / static_cast<double>(tokens) : 0.0;
        const double avg_first_token_latency =
            first_token_samples > 0 && first_token_latency_sum > 0 ?
                static_cast<double>(first_token_latency_sum) / static_cast<double>(first_token_samples) :
                0.0;
        const double tokens_per_second = compute_tokens_per_second(output_tokens, decode_latency_sum);
        json += "{\"bucket\":\"" + json_escape(bucket) + "\",\"usd\":" + decimal_string(usd, 6) +
                ",\"tokens\":" + std::to_string(tokens) + ",\"cache_ratio\":" + decimal_string(cache_ratio * 100.0, 1) +
                ",\"avg_first_token_latency\":" + decimal_string(avg_first_token_latency, 1) +
                ",\"tokens_per_second\":" + decimal_string(tokens_per_second, 2) + "}";
    }
    json += "]}";
    return json;
}

HttpResponse channels_page_response(std::string_view raw_request, const ParsedRequest &parsed,
                                    std::string_view request_id)
{
    RootAuth auth = authenticate_root_admin(raw_request);
    if (!auth.ok) {
        return admin_auth_failure(request_id, auth.failure, auth.clear_cookie, raw_request);
    }
    ChannelPageWindow window;
    std::string error;
    if (!parse_channel_page_window(parsed.target, window, error)) {
        return api_json_response(api_failure(error), { { "X-Request-Id", std::string{ request_id } } });
    }

    try {
        return api_json_response(api_success(*parse_json(channels_page_json(window))),
                                 { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse channel_time_series_response(std::string_view raw_request, const ParsedRequest &parsed,
                                          std::string_view request_id)
{
    RootAuth auth = authenticate_root_admin(raw_request);
    if (!auth.ok) {
        return admin_auth_failure(request_id, auth.failure, auth.clear_cookie, raw_request);
    }
    ChannelTimeSeriesRequest req;
    std::string error;
    if (!parse_channel_time_series_request(parsed, req, error)) {
        return api_json_response(api_failure(error), { { "X-Request-Id", std::string{ request_id } } });
    }

    try {
        return api_json_response(api_success(*parse_json(channel_time_series_json(req))),
                                 { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse create_channel_response(std::string_view raw_request, std::string_view body, std::string_view request_id)
{
    RootAuth auth = authenticate_root_admin(raw_request);
    if (!auth.ok) {
        return admin_auth_failure(request_id, auth.failure, auth.clear_cookie, raw_request);
    }
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
    }

    try {
        Channel channel;
        const boost::json::value *type_value = object->if_contains("type");
        const auto type = parse_int_value(type_value != nullptr ? json_value_to_string(*type_value) : std::string{});
        if (!type.has_value() || *type <= 0) {
            return api_json_response(api_failure("type 无效"), { { "X-Request-Id", std::string{ request_id } } });
        }
        channel.type = *type;
        channel.name = trim_ascii(json_object_string(*object, "name"));
        const boost::json::value *status_value = object->if_contains("status");
        channel.status = parse_bool_value(status_value != nullptr ? json_value_to_string(*status_value) : std::string{})
                             .value_or(true);
        const boost::json::value *priority_value = object->if_contains("priority");
        channel.priority =
            parse_int_value(priority_value != nullptr ? json_value_to_string(*priority_value) : std::string{})
                .value_or(0);
        channel.base_url = trim_ascii(json_object_string(*object, "base_url"));
        channel.api_key = trim_ascii(json_object_string(*object, "key"));
        const boost::json::value *price_value = object->if_contains("price_multiplier");
        if (price_value != nullptr) {
            if (!price_value->is_number()) {
                return api_json_response(api_failure("price_multiplier 无效"),
                                         { { "X-Request-Id", std::string{ request_id } } });
            }
            channel.price_multiplier = price_value->to_number<double>();
            if (!(channel.price_multiplier > 0.0)) {
                return api_json_response(api_failure("price_multiplier 必须大于 0"),
                                         { { "X-Request-Id", std::string{ request_id } } });
            }
        }
        if (channel.name.empty()) {
            return api_json_response(api_failure("name 不能为空"), { { "X-Request-Id", std::string{ request_id } } });
        }
        if (channel.base_url.empty()) {
            return api_json_response(api_failure("base_url 不能为空"),
                                     { { "X-Request-Id", std::string{ request_id } } });
        }

        ChannelStore &store = ChannelStore::instance();
        if (!store.create_channel(channel)) {
            return api_json_response(api_failure("创建渠道失败"), { { "X-Request-Id", std::string{ request_id } } });
        }
        return api_json_response(api_success(boost::json::object{ { "id", channel.id } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse update_channel_response(std::string_view raw_request, std::string_view body, std::string_view request_id)
{
    RootAuth auth = authenticate_root_admin(raw_request);
    if (!auth.ok) {
        return admin_auth_failure(request_id, auth.failure, auth.clear_cookie, raw_request);
    }
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
    }

    const boost::json::value *id_value = object->if_contains("id");
    const auto channel_id = parse_long_long(id_value != nullptr ? json_value_to_string(*id_value) : std::string{});
    if (!channel_id.has_value() || *channel_id <= 0) {
        return api_json_response(api_failure("渠道 ID 无效"), { { "X-Request-Id", std::string{ request_id } } });
    }

    try {
        ChannelStore &store = ChannelStore::instance();
        auto channel = find_channel(store, *channel_id);
        if (!channel.has_value()) {
            return api_json_response(api_failure("渠道不存在"), { { "X-Request-Id", std::string{ request_id } } });
        }

        const std::string name = trim_ascii(json_object_string(*object, "name"));
        if (!name.empty()) {
            channel->name = name;
        }
        if (const boost::json::value *status_value = object->if_contains("status"); status_value != nullptr) {
            if (const auto status = parse_bool_value(json_value_to_string(*status_value)); status.has_value()) {
                channel->status = *status;
            }
        }
        if (const boost::json::value *priority_value = object->if_contains("priority"); priority_value != nullptr) {
            if (const auto priority = parse_int_value(json_value_to_string(*priority_value)); priority.has_value()) {
                channel->priority = *priority;
            }
        }
        const std::string base_url = trim_ascii(json_object_string(*object, "base_url"));
        if (!base_url.empty()) {
            channel->base_url = base_url;
        }
        if (object->if_contains("key") != nullptr) {
            channel->api_key = trim_ascii(json_object_string(*object, "key"));
        }
        if (const boost::json::value *price_value = object->if_contains("price_multiplier"); price_value != nullptr) {
            if (!price_value->is_number()) {
                return api_json_response(api_failure("price_multiplier 无效"),
                                         { { "X-Request-Id", std::string{ request_id } } });
            }
            const double price_multiplier = price_value->to_number<double>();
            if (!(price_multiplier > 0.0)) {
                return api_json_response(api_failure("price_multiplier 必须大于 0"),
                                         { { "X-Request-Id", std::string{ request_id } } });
            }
            channel->price_multiplier = price_multiplier;
        }

        if (!store.update_channel(*channel)) {
            return api_json_response(api_failure("渠道不存在"), { { "X-Request-Id", std::string{ request_id } } });
        }
        return api_json_response(api_success(), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), { { "X-Request-Id", std::string{ request_id } } });
    }
}

std::optional<long long> path_channel_id_for_prefix_suffix(std::string_view path, std::string_view prefix,
                                                           std::string_view suffix)
{
    if (!path.starts_with(prefix) || !path.ends_with(suffix) || path.size() <= prefix.size() + suffix.size()) {
        return std::nullopt;
    }
    return parse_long_long(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
}

HttpResponse delete_channel_response(std::string_view raw_request, long long channel_id, std::string_view request_id)
{
    RootAuth auth = authenticate_root_admin(raw_request);
    if (!auth.ok) {
        return admin_auth_failure(request_id, auth.failure, auth.clear_cookie, raw_request);
    }
    try {
        ChannelStore &store = ChannelStore::instance();
        Channel channel;
        channel.id = channel_id;
        if (!store.delete_channel(channel)) {
            return api_json_response(api_failure("渠道不存在"), { { "X-Request-Id", std::string{ request_id } } });
        }
        return api_json_response(api_success(), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), { { "X-Request-Id", std::string{ request_id } } });
    }
}

bool channel_admin_dispatch(std::string_view raw_request, std::string_view body,
                            const ChannelAdminParsedRequest &parsed, std::string_view request_id, HttpResponse &out)
{
    ParsedRequest legacy{ parsed.method, parsed.path, parsed.target };
    const auto &method = parsed.method;
    const auto &path = parsed.path;

    if (method == "GET" && path == "/api/channel/page") {
        out = channels_page_response(raw_request, legacy, request_id);
        return true;
    }
    if (method == "POST" && path == "/api/channel") {
        out = create_channel_response(raw_request, body, request_id);
        return true;
    }
    if (method == "PUT" && path == "/api/channel") {
        out = update_channel_response(raw_request, body, request_id);
        return true;
    }
    if (method == "DELETE" && path.rfind("/api/channel/", 0) == 0) {
        if (auto channel_id = parse_long_long(path.substr(std::string_view("/api/channel/").size()));
            channel_id.has_value() && *channel_id > 0) {
            out = delete_channel_response(raw_request, *channel_id, request_id);
            return true;
        }
    }
    if (method == "GET" && path.ends_with("/timeseries") && path.rfind("/api/channel/", 0) == 0) {
        out = channel_time_series_response(raw_request, legacy, request_id);
        return true;
    }
    return false;
}

HttpResponse channel_admin_route(std::string_view raw_request, std::string_view body,
                                 const ChannelAdminParsedRequest &parsed, std::string_view request_id)
{
    HttpResponse out;
    if (channel_admin_dispatch(raw_request, body, parsed, request_id, out)) {
        return out;
    }
    return http_response(404, "Not Found", boost::json::value("not found"),
                         { { "X-Request-Id", std::string{ request_id } } });
}

} // namespace revlm
