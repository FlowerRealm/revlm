#include "usage/user_usage_api.hpp"

#include "auth/session.hpp"
#include "auth/users.hpp"
#include "models/models.hpp"
#include "server/http_server.hpp"
#include "store/mysql.hpp"
#include "request/request.hpp"
#include "util/http_query.hpp"
#include "util/user_input.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

struct ParsedTarget {
    std::string_view target;
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
            out.push_back(ch);
            break;
        }
    }
    return out;
}

std::string api_success(std::string_view data_json = {})
{
    if (data_json.empty())
        return "{\"success\":true,\"message\":\"\",\"data\":null}\n";
    return std::string{ "{\"success\":true,\"message\":\"\",\"data\":" } + std::string{ data_json } + "}\n";
}

std::string api_failure(std::string_view message)
{
    return std::string{ "{\"success\":false,\"message\":\"" } + json_escape(message) + "\"}\n";
}

HttpResponse api_json_response(std::string body, std::string_view request_id, const std::vector<Header> &headers = {})
{
    return http_response(200, "OK", std::move(body), "application/json; charset=utf-8", request_id, headers);
}

std::string clear_session_cookie_header(std::string_view raw_request)
{
    (void)raw_request;
    return "revlm_session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0";
}

std::optional<User> require_user(std::string_view raw_request, const Config &config, std::string_view request_id,
                                 HttpResponse &auth_response)
{
    const WebSessionAuth auth = authenticate_web_session(raw_request, config);
    if (auth.ok && auth.user.has_value())
        return auth.user;
    std::vector<Header> headers;
    if (auth.clear_cookie)
        headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
    auth_response = api_json_response(api_failure(auth.failure_message.empty() ? "未登录" : auth.failure_message),
                                      request_id, headers);
    return std::nullopt;
}

struct UsageWindowSummary {
    std::string since;
    std::string until;
    long long requests = 0;
    long long tokens = 0;
    long long rpm = 0;
    long long tpm = 0;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_tokens = 0;
    long long cache_creation_tokens = 0;
    double cache_ratio = 0.0;
    long long first_token_samples = 0;
    double avg_first_token_latency = 0.0;
    double tokens_per_second = 0.0;
    std::string used_usd = "0";
    std::string committed_usd = "0";
    std::string limit_usd = "0";
    std::string remaining_usd = "0";
};

struct DashboardModelStat {
    std::string model;
    std::string icon_url;
    std::string color;
    long long requests = 0;
    long long tokens = 0;
    std::string committed_usd;
};

struct UsageSeriesPoint {
    std::string bucket;
    long long requests = 0;
    long long tokens = 0;
    double committed_usd = 0.0;
    double cache_ratio = 0.0;
    double avg_first_token_latency = 0.0;
    double tokens_per_second = 0.0;
};

struct UserEventRow {
    long long id = 0;
    std::string time;
    std::optional<std::string> endpoint;
    std::optional<std::string> method;
    long long token_id = 0;
    long long channel_id = 0;
    std::string status;
    std::optional<std::string> model;
    std::optional<std::string> service_tier;
    std::optional<long long> input_tokens;
    std::optional<long long> cache_read_tokens;
    std::optional<long long> cache_creation_5m_tokens;
    std::optional<long long> cache_creation_1h_tokens;
    std::optional<long long> output_tokens;
    std::string committed_usd = "0.000000";
    int status_code = 0;
    int latency_ms = 0;
    std::optional<std::string> error_class;
    std::optional<std::string> error_message;
    bool is_stream = false;
};

struct UserEventsPage {
    std::vector<UserEventRow> events;
    std::optional<long long> next_before_id;
};

struct UserEventPricingBreakdown {
    std::optional<std::string> model_public_id;
    bool model_found = false;
    std::optional<std::string> owned_by;
    std::optional<std::string> service_tier;
    std::string pricing_kind = "base";
    long long input_tokens_total = 0;
    long long input_tokens_cache_read = 0;
    long long input_tokens_cache_creation = 0;
    long long input_tokens_cache_creation_5m = 0;
    long long input_tokens_cache_creation_1h = 0;
    long long input_tokens_billable = 0;
    long long output_tokens_total = 0;
    std::string input_usd_per_1m = "0.000000";
    std::string output_usd_per_1m = "0.000000";
    std::string cache_read_usd_per_1m = "0.000000";
    std::string cache_creation_5m_usd_per_1m = "0.000000";
    std::string cache_creation_1h_usd_per_1m = "0.000000";
    std::string input_cost_usd = "0.000000";
    std::string output_cost_usd = "0.000000";
    std::string cache_read_cost_usd = "0.000000";
    std::string cache_creation_cost_usd = "0.000000";
    std::string cache_creation_5m_cost_usd = "0.000000";
    std::string cache_creation_1h_cost_usd = "0.000000";
    std::string base_cost_usd = "0.000000";
    double tier_multiplier = 1.0;
    double channel_multiplier = 1.0;
    std::string final_cost_usd = "0.000000";
};

struct UserEventDetail {
    long long event_id = 0;
    std::optional<UserEventPricingBreakdown> pricing_breakdown;
};

struct WindowRollupTotals {
    long long requests = 0;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_tokens = 0;
    long long cache_creation_tokens = 0;
    long long first_token_latency_sum = 0;
    double usd = 0.0;
};

bool valid_timezone_name(std::string_view raw)
{
    if (raw.empty() || raw.size() > 64) {
        return false;
    }
    if (raw.front() == '/' || raw.back() == '/') {
        return false;
    }
    bool prev_slash = false;
    for (char ch : raw) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '/' || ch == '_' || ch == '-' ||
                        ch == '+';
        if (!ok) {
            return false;
        }
        if (ch == '/') {
            if (prev_slash) {
                return false;
            }
            prev_slash = true;
        } else {
            prev_slash = false;
        }
    }
    static constexpr std::string_view kZoneinfoRoots[] = {
        "/usr/share/zoneinfo",
        "/usr/share/lib/zoneinfo",
    };
    std::error_code ec;
    for (std::string_view root : kZoneinfoRoots) {
        std::filesystem::path path{ root };
        path /= std::string{ raw };
        ec.clear();
        if (!std::filesystem::exists(path, ec) || ec) {
            continue;
        }
        ec.clear();
        if (!std::filesystem::is_directory(path, ec) && !ec) {
            return true;
        }
    }
    return false;
}

time_t timegm_utc(std::tm tm)
{
#if defined(__USE_MISC) || defined(__APPLE__) || defined(__FreeBSD__)
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

std::string iso8601_from_unix(time_t seconds)
{
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
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

    ScopedTimezone(const ScopedTimezone &) = delete;
    ScopedTimezone &operator=(const ScopedTimezone &) = delete;

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

bool parse_mysql_datetime_utc(std::string_view raw, time_t &out)
{
    if (raw.size() < 19) {
        return false;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    try {
        year = std::stoi(std::string{ raw.substr(0, 4) });
        month = std::stoi(std::string{ raw.substr(5, 2) });
        day = std::stoi(std::string{ raw.substr(8, 2) });
        hour = std::stoi(std::string{ raw.substr(11, 2) });
        minute = std::stoi(std::string{ raw.substr(14, 2) });
        second = std::stoi(std::string{ raw.substr(17, 2) });
    } catch (const std::exception &) {
        return false;
    }
    if (raw[4] != '-' || raw[7] != '-' || raw[10] != ' ' || raw[13] != ':' || raw[16] != ':') {
        return false;
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;
    out = timegm_utc(tm);
    return true;
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

void next_date(int &year, int &month, int &day)
{
    ++day;
    const int dim = days_in_month(year, month);
    if (day <= dim) {
        return;
    }
    day = 1;
    ++month;
    if (month <= 12) {
        return;
    }
    month = 1;
    ++year;
}

std::tm local_tm_from_utc(time_t utc, std::string_view time_zone)
{
    ScopedTimezone scoped(time_zone);
    std::tm tm{};
    localtime_r(&utc, &tm);
    return tm;
}

std::string iso8601_hour_bucket_from_utc(time_t utc, std::string_view time_zone)
{
    const std::tm tm = local_tm_from_utc(utc, time_zone);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:00:00", &tm);
    return buffer;
}

std::string iso8601_day_bucket_from_utc(time_t utc, std::string_view time_zone)
{
    const std::tm tm = local_tm_from_utc(utc, time_zone);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm);
    return buffer;
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

long long parse_i64_or_zero(const std::optional<std::string> &raw)
{
    if (!raw.has_value())
        return 0;
    long long value = 0;
    return parse_i64(*raw, value) ? value : 0;
}

double parse_decimal_or_zero(std::string_view raw)
{
    const std::string value = trim_ascii(raw);
    if (value.empty())
        return 0.0;
    try {
        return std::stod(value);
    } catch (...) {
        return 0.0;
    }
}

std::string decimal_to_string(double value)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(6);
    out << std::max(0.0, value);
    return out.str();
}

std::string subtract_decimal(std::string_view limit, std::string_view used)
{
    return decimal_to_string(parse_decimal_or_zero(limit) - parse_decimal_or_zero(used));
}

std::string mysql_datetime_to_iso8601_z(std::string_view raw)
{
    if (raw.empty()) {
        return {};
    }
    std::string out{ raw };
    if (out.size() >= 19 && out[10] == ' ') {
        out[10] = 'T';
    }
    if (!out.empty() && out.back() != 'Z') {
        out += 'Z';
    }
    return out;
}

struct UsageQueryOptions {
    std::string time_zone;
    bool all_time = false;
    std::optional<time_t> start_utc;
    std::optional<time_t> end_exclusive_utc;
    std::string start_date;
    std::string end_date;
    std::optional<long long> token_id;
};

bool parse_usage_query_options(const std::map<std::string, std::string> &params, UsageQueryOptions &out,
                               std::string &message)
{
    out = UsageQueryOptions{};
    out.time_zone = trim_ascii(query_param_value(params, "tz"));
    if (out.time_zone.empty()) {
        out.time_zone = "UTC";
    }
    if (!valid_timezone_name(out.time_zone)) {
        message = "tz 无效";
        return false;
    }

    const std::string all_time_raw = trim_ascii(query_param_value(params, "all_time"));
    if (!all_time_raw.empty()) {
        bool all_time = false;
        if (!parse_bool_flag(all_time_raw, all_time)) {
            message = "all_time 无效";
            return false;
        }
        out.all_time = all_time;
    }

    const std::string start = trim_ascii(query_param_value(params, "start"));
    const std::string end = trim_ascii(query_param_value(params, "end"));
    out.start_date = start;
    out.end_date = end;
    if (!start.empty()) {
        int y = 0;
        int m = 0;
        int d = 0;
        if (!parse_date_yyyy_mm_dd(start, y, m, d)) {
            message = "start 无效";
            return false;
        }
        out.start_utc = utc_seconds_from_local_date(y, m, d, out.time_zone);
    }
    if (!end.empty()) {
        int y = 0;
        int m = 0;
        int d = 0;
        if (!parse_date_yyyy_mm_dd(end, y, m, d)) {
            message = "end 无效";
            return false;
        }
        const time_t end_start = utc_seconds_from_local_date(y, m, d, out.time_zone);
        next_date(y, m, d);
        out.end_exclusive_utc = utc_seconds_from_local_date(y, m, d, out.time_zone);
        if (!out.end_exclusive_utc.has_value() || *out.end_exclusive_utc <= end_start) {
            out.end_exclusive_utc = end_start + 86400;
        }
    }
    if (out.start_utc.has_value() && out.end_exclusive_utc.has_value() && *out.start_utc >= *out.end_exclusive_utc) {
        message = "日期范围无效";
        return false;
    }

    const std::string token_id_raw = trim_ascii(query_param_value(params, "token_id"));
    if (!token_id_raw.empty()) {
        long long token_id = 0;
        if (!parse_i64(token_id_raw, token_id) || token_id <= 0) {
            message = "token_id 无效";
            return false;
        }
        out.token_id = token_id;
    }
    return true;
}

std::string usage_where_clause(long long user_id, const UsageQueryOptions &options, MysqlConnection &conn,
                               bool committed_only = false)
{
    std::string sql = " WHERE user_id=" + std::to_string(user_id);
    if (committed_only) {
        sql += " AND status='committed'";
    }
    if (options.token_id.has_value()) {
        sql += " AND token_id=" + std::to_string(*options.token_id);
    }
    if (!options.all_time) {
        if (options.start_utc.has_value()) {
            sql += " AND `time`>=" + conn.quote(mysql_datetime_from_unix(*options.start_utc));
        }
        if (options.end_exclusive_utc.has_value()) {
            sql += " AND `time`<" + conn.quote(mysql_datetime_from_unix(*options.end_exclusive_utc));
        }
    }
    return sql;
}

std::string model_icon_url(std::string_view owned_by)
{
    const std::string owner = lowercase_ascii(trim_ascii(owned_by));
    if (owner.empty()) {
        return {};
    }
    return "/assets/model-icons/" + owner +
           (owner == "openai" || owner == "xai" || owner == "openrouter" || owner == "ollama" ? ".svg" : "-color.svg");
}

std::string user_models_json(const std::vector<Model> &models)
{
    auto price_string = [](double price) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.6f", price);
        return std::string{ buffer };
    };
    std::string json = "[";
    for (size_t i = 0; i < models.size(); ++i) {
        const Model &model = models[i];
        if (i > 0) {
            json += ",";
        }
        json += "{\"id\":" + std::to_string(model.id) + ",\"public_id\":\"" + json_escape(model.name) +
                "\",\"group_name\":\"\"" + ",\"owned_by\":\"" + json_escape(model.owned_by) +
                "\",\"input_usd_per_1m\":\"" + json_escape(price_string(model.input_price)) +
                "\",\"output_usd_per_1m\":\"" + json_escape(price_string(model.output_price)) +
                "\",\"cache_read_input_usd_per_1m\":\"" + json_escape(price_string(model.cache_read_price)) +
                "\",\"cache_creation_input_usd_per_1m\":\"" + json_escape(price_string(model.cache_creation_5m_price)) +
                "\",\"cache_creation_1h_input_usd_per_1m\":\"" +
                json_escape(price_string(model.cache_creation_1h_price)) + "\",\"status\":1" + ",\"icon_url\":\"" +
                json_escape(model_icon_url(model.owned_by)) + "\"}";
    }
    json += "]";
    return json;
}

std::string json_string_or_null(const std::optional<std::string> &value)
{
    if (!value.has_value()) {
        return "null";
    }
    return "\"" + json_escape(*value) + "\"";
}

std::string json_string(std::string_view value)
{
    return "\"" + json_escape(value) + "\"";
}

std::string bool_json(bool value)
{
    return value ? "true" : "false";
}

std::string token_json_number_or_null(const std::optional<long long> &value)
{
    if (!value.has_value()) {
        return "null";
    }
    return std::to_string(*value);
}

std::string mysql_to_iso_utc(std::string_view value)
{
    if (value.empty()) {
        return {};
    }
    std::string out{ value };
    if (out.size() == 19) {
        for (char &ch : out) {
            if (ch == ' ') {
                ch = 'T';
                break;
            }
        }
        out += "Z";
    }
    return out;
}

std::string status_json_label(std::string_view status)
{
    if (status == "committed" || status == "1" || status == "true" || status == "TRUE") {
        return "committed";
    }
    return std::string{ status };
}

std::string price_string(double price)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6f", price);
    return std::string{ buffer };
}

double parse_decimal(std::string_view raw)
{
    try {
        return std::stod(std::string{ raw });
    } catch (const std::exception &) {
        return 0.0;
    }
}

UserEventRow request_to_user_event(const Request &req)
{
    UserEventRow out;
    out.id = req.id;
    out.time = mysql_to_iso_utc(req.time);
    out.endpoint = req.endpoint.empty() ? std::nullopt : std::optional<std::string>{ req.endpoint };
    out.method = req.method.empty() ? std::nullopt : std::optional<std::string>{ req.method };
    out.token_id = req.token_id;
    out.channel_id = req.channel_id;
    out.status = status_json_label(req.status.empty() ? (req.statue ? "committed" : "") : req.status);
    out.model = req.model.name.empty() ? std::nullopt : std::optional<std::string>{ req.model.name };
    out.service_tier = req.service_tier.empty() ? std::nullopt : std::optional<std::string>{ req.service_tier };
    out.input_tokens = req.input_tokens;
    out.cache_read_tokens = req.cache_read_tokens;
    out.cache_creation_5m_tokens = req.cache_creation_5m_tokens;
    out.cache_creation_1h_tokens = req.cache_creation_1h_tokens;
    out.output_tokens = req.output_tokens;
    out.status_code = req.status_code;
    out.latency_ms = req.latency_ms;
    out.error_class = req.error_class.empty() ? std::nullopt : std::optional<std::string>{ req.error_class };
    out.error_message = req.error_message.empty() ? std::nullopt : std::optional<std::string>{ req.error_message };
    out.is_stream = req.is_stream;
    Request priced = req;
    hydrate_request_model(priced);
    out.committed_usd = decimal_to_string(priced.solve_price());
    return out;
}

std::optional<UserEventDetail> request_to_user_event_detail(const Request &req)
{
    Request hydrated = req;
    hydrate_request_model(hydrated);
    UserEventDetail detail;
    detail.event_id = hydrated.id;
    UserEventPricingBreakdown pricing;
    const std::string model_id = trim_ascii(hydrated.model.name);
    const std::vector<Model> &models = ModelManager::instance().models();
    const auto builtin_it = std::ranges::find(models, model_id, &Model::name);
    const bool found = builtin_it != models.end();
    pricing.model_public_id = model_id.empty() ? std::nullopt : std::optional<std::string>{ model_id };
    pricing.model_found = found;
    pricing.owned_by =
        found ? std::optional<std::string>{ builtin_it->owned_by } : std::optional<std::string>{ "openai" };
    pricing.service_tier =
        hydrated.service_tier.empty() ? std::nullopt : std::optional<std::string>{ hydrated.service_tier };
    pricing.input_tokens_total = hydrated.input_tokens;
    pricing.input_tokens_cache_read = hydrated.cache_read_tokens;
    pricing.input_tokens_cache_creation_5m = hydrated.cache_creation_5m_tokens;
    pricing.input_tokens_cache_creation_1h = hydrated.cache_creation_1h_tokens;
    pricing.input_tokens_cache_creation = hydrated.cache_creation_5m_tokens + hydrated.cache_creation_1h_tokens;
    pricing.output_tokens_total = hydrated.output_tokens;
    pricing.input_tokens_billable = std::max(0, hydrated.input_tokens - hydrated.cache_read_tokens -
                                                    hydrated.cache_creation_5m_tokens -
                                                    hydrated.cache_creation_1h_tokens);
    pricing.input_usd_per_1m = found ? price_string(builtin_it->input_price) : "0.000000";
    pricing.output_usd_per_1m = found ? price_string(builtin_it->output_price) : "0.000000";
    pricing.cache_read_usd_per_1m = found ? price_string(builtin_it->cache_read_price) : "0.000000";
    pricing.cache_creation_5m_usd_per_1m = found ? price_string(builtin_it->cache_creation_5m_price) : "0.000000";
    pricing.cache_creation_1h_usd_per_1m = found ? price_string(builtin_it->cache_creation_1h_price) : "0.000000";
    const double input_rate = parse_decimal(pricing.input_usd_per_1m) / 1000000.0;
    const double output_rate = parse_decimal(pricing.output_usd_per_1m) / 1000000.0;
    const double cache_read_rate = parse_decimal(pricing.cache_read_usd_per_1m) / 1000000.0;
    const double cache_create_5m_rate = parse_decimal(pricing.cache_creation_5m_usd_per_1m) / 1000000.0;
    const double cache_create_1h_rate = parse_decimal(pricing.cache_creation_1h_usd_per_1m) / 1000000.0;
    const double input_cost = static_cast<double>(pricing.input_tokens_billable) * input_rate;
    const double output_cost = static_cast<double>(pricing.output_tokens_total) * output_rate;
    const double cache_read_cost = static_cast<double>(pricing.input_tokens_cache_read) * cache_read_rate;
    const double cache_create_total_cost =
        static_cast<double>(pricing.input_tokens_cache_creation_5m) * cache_create_5m_rate +
        static_cast<double>(pricing.input_tokens_cache_creation_1h) * cache_create_1h_rate;
    pricing.input_cost_usd = decimal_to_string(input_cost);
    pricing.output_cost_usd = decimal_to_string(output_cost);
    pricing.cache_read_cost_usd = decimal_to_string(cache_read_cost);
    pricing.cache_creation_cost_usd = decimal_to_string(cache_create_total_cost);
    pricing.cache_creation_5m_cost_usd =
        decimal_to_string(static_cast<double>(pricing.input_tokens_cache_creation_5m) * cache_create_5m_rate);
    pricing.cache_creation_1h_cost_usd =
        decimal_to_string(static_cast<double>(pricing.input_tokens_cache_creation_1h) * cache_create_1h_rate);
    pricing.base_cost_usd = decimal_to_string(input_cost + output_cost + cache_read_cost + cache_create_total_cost);
    pricing.tier_multiplier = hydrated.tier_multiplier;
    pricing.channel_multiplier = hydrated.channel_multiplier;
    pricing.final_cost_usd = decimal_to_string(hydrated.solve_price());
    detail.pricing_breakdown = pricing;
    return detail;
}

std::string usage_event_to_user_json(const UserEventRow &event)
{
    const long long cache_creation =
        event.cache_creation_5m_tokens.value_or(0) + event.cache_creation_1h_tokens.value_or(0);
    std::string out = "{";
    out += "\"id\":" + std::to_string(event.id);
    out += ",\"time\":" + json_string(event.time);
    out += ",\"request_id\":" + json_string(std::to_string(event.id));
    out += ",\"endpoint\":" + json_string_or_null(event.endpoint);
    out += ",\"method\":" + json_string_or_null(event.method);
    out += ",\"token_id\":" + std::to_string(event.token_id);
    out += ",\"channel_id\":" + (event.channel_id > 0 ? std::to_string(event.channel_id) : "null");
    out += ",\"status\":" + json_string(event.status);
    out += ",\"model\":" + json_string_or_null(event.model);
    out += ",\"service_tier\":" + json_string_or_null(event.service_tier);
    out += ",\"input_tokens\":" + token_json_number_or_null(event.input_tokens);
    out += ",\"cache_read_tokens\":" + token_json_number_or_null(event.cache_read_tokens);
    out += ",\"cache_creation_5m_tokens\":" + token_json_number_or_null(event.cache_creation_5m_tokens);
    out += ",\"cache_creation_1h_tokens\":" + token_json_number_or_null(event.cache_creation_1h_tokens);
    out += ",\"cache_creation_tokens\":" + std::to_string(cache_creation);
    out += ",\"output_tokens\":" + token_json_number_or_null(event.output_tokens);
    out += ",\"committed_usd\":" + json_string(event.committed_usd);
    out += ",\"status_code\":" + std::to_string(event.status_code);
    out += ",\"latency_ms\":" + std::to_string(event.latency_ms);
    out += ",\"error_class\":" + json_string_or_null(event.error_class);
    out += ",\"error_message\":" + json_string_or_null(event.error_message);
    out += ",\"is_stream\":" + bool_json(event.is_stream);
    out += "}";
    return out;
}

std::string requests_page_to_user_json(const UserEventsPage &page)
{
    std::string out = "{\"events\":[";
    for (size_t i = 0; i < page.events.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += usage_event_to_user_json(page.events[i]);
    }
    out += "]";
    if (page.next_before_id.has_value()) {
        out += ",\"next_before_id\":" + std::to_string(*page.next_before_id);
    } else {
        out += ",\"next_before_id\":null";
    }
    out += "}";
    return out;
}

std::string usage_event_detail_to_json(const UserEventDetail &detail)
{
    std::string out = "{\"event_id\":" + std::to_string(detail.event_id);
    if (detail.pricing_breakdown.has_value()) {
        const auto &p = *detail.pricing_breakdown;
        out += ",\"pricing_breakdown\":{";
        out += "\"model_public_id\":" + json_string_or_null(p.model_public_id);
        out += ",\"model_found\":" + bool_json(p.model_found);
        out += ",\"owned_by\":" + json_string_or_null(p.owned_by);
        out += ",\"service_tier\":" + json_string_or_null(p.service_tier);
        out += ",\"pricing_kind\":" + json_string(p.pricing_kind);
        out += ",\"input_tokens_total\":" + std::to_string(p.input_tokens_total);
        out += ",\"input_tokens_cache_read\":" + std::to_string(p.input_tokens_cache_read);
        out += ",\"input_tokens_cache_creation\":" + std::to_string(p.input_tokens_cache_creation);
        out += ",\"input_tokens_cache_creation_5m\":" + std::to_string(p.input_tokens_cache_creation_5m);
        out += ",\"input_tokens_cache_creation_1h\":" + std::to_string(p.input_tokens_cache_creation_1h);
        out += ",\"input_tokens_billable\":" + std::to_string(p.input_tokens_billable);
        out += ",\"output_tokens_total\":" + std::to_string(p.output_tokens_total);
        out += ",\"input_usd_per_1m\":" + json_string(p.input_usd_per_1m);
        out += ",\"output_usd_per_1m\":" + json_string(p.output_usd_per_1m);
        out += ",\"cache_read_usd_per_1m\":" + json_string(p.cache_read_usd_per_1m);
        out += ",\"cache_creation_5m_usd_per_1m\":" + json_string(p.cache_creation_5m_usd_per_1m);
        out += ",\"cache_creation_1h_usd_per_1m\":" + json_string(p.cache_creation_1h_usd_per_1m);
        out += ",\"input_cost_usd\":" + json_string(p.input_cost_usd);
        out += ",\"output_cost_usd\":" + json_string(p.output_cost_usd);
        out += ",\"cache_read_cost_usd\":" + json_string(p.cache_read_cost_usd);
        out += ",\"cache_creation_cost_usd\":" + json_string(p.cache_creation_cost_usd);
        out += ",\"cache_creation_5m_cost_usd\":" + json_string(p.cache_creation_5m_cost_usd);
        out += ",\"cache_creation_1h_cost_usd\":" + json_string(p.cache_creation_1h_cost_usd);
        out += ",\"base_cost_usd\":" + json_string(p.base_cost_usd);
        {
            char tier_buf[32];
            char channel_buf[32];
            std::snprintf(tier_buf, sizeof(tier_buf), "%.6f", p.tier_multiplier);
            std::snprintf(channel_buf, sizeof(channel_buf), "%.6f", p.channel_multiplier);
            out += ",\"tier_multiplier\":";
            out += tier_buf;
            out += ",\"channel_multiplier\":";
            out += channel_buf;
        }
        out += ",\"final_cost_usd\":" + json_string(p.final_cost_usd);
        out += "}";
    }
    out += "}";
    return out;
}

std::string utc_date_from_unix(long long unix_seconds)
{
    return mysql_datetime_from_unix(unix_seconds).substr(0, 10);
}

WindowRollupTotals sum_user_request_totals(MysqlConnection &conn, long long user_id,
                                         const std::optional<long long> &token_id, const std::string &start_date,
                                         const std::string &end_date)
{
    UserStore &users = UserStore::instance();
    users.reload(conn);
    TokenStore &tokens = users.tokens();
    WindowRollupTotals out;
    for (const UserToken &tok : tokens.list_user_tokens(user_id)) {
        if (token_id.has_value() && tok.id != *token_id) {
            continue;
        }
        for (const RequestTotal &row : tokens.requests(tok.id).totals(start_date, end_date)) {
            out.requests += row.requests;
            out.input_tokens += row.input_tokens;
            out.output_tokens += row.output_tokens;
            out.cache_read_tokens += row.cache_read_tokens;
            out.cache_creation_tokens += row.cache_creation_tokens;
            out.first_token_latency_sum += row.first_token_latency_sum;
            out.usd += row.usd;
        }
    }
    return out;
}

bool usage_window_has_bounded_range(const UsageQueryOptions &options)
{
    return !options.all_time && options.start_utc.has_value() && options.end_exclusive_utc.has_value() &&
           *options.end_exclusive_utc > *options.start_utc;
}

void apply_usage_totals_to_window_summary(UsageWindowSummary &summary, const WindowRollupTotals &totals)
{
    summary.requests = totals.requests;
    summary.input_tokens = totals.input_tokens;
    summary.output_tokens = totals.output_tokens;
    summary.cache_read_tokens = totals.cache_read_tokens;
    summary.cache_creation_tokens = totals.cache_creation_tokens;
    summary.tokens = totals.input_tokens + totals.output_tokens + totals.cache_read_tokens +
                     totals.cache_creation_tokens;
    if (totals.requests > 0 && totals.first_token_latency_sum > 0) {
        summary.first_token_samples = totals.requests;
        summary.avg_first_token_latency =
            static_cast<double>(totals.first_token_latency_sum) / static_cast<double>(totals.requests);
    }
    const long long cached = totals.cache_read_tokens + totals.cache_creation_tokens;
    if (totals.input_tokens > 0) {
        summary.cache_ratio = static_cast<double>(cached) / static_cast<double>(totals.input_tokens);
    }
}

void finalize_usage_window_rates(UsageWindowSummary &summary, const UsageQueryOptions &options, time_t window_start,
                                 time_t window_end)
{
    if (summary.since.empty() || summary.until.empty()) {
        return;
    }
    double minutes = 1.0;
    if (options.start_utc.has_value() && options.end_exclusive_utc.has_value()) {
        minutes = std::max(1.0, std::difftime(*options.end_exclusive_utc, *options.start_utc) / 60.0);
    } else if (window_end >= window_start) {
        minutes = std::max(1.0, std::difftime(window_end, window_start) / 60.0 + 1.0 / 60.0);
    }
    summary.rpm = static_cast<long long>(std::llround(static_cast<double>(summary.requests) / minutes));
    summary.tpm = static_cast<long long>(std::llround(static_cast<double>(summary.tokens) / minutes));
}

UsageWindowSummary query_usage_window_summary(MysqlConnection &conn, long long user_id,
                                              const UsageQueryOptions &options)
{
    UsageWindowSummary summary;
    time_t window_start = options.start_utc.value_or(0);
    time_t window_end = options.end_exclusive_utc.has_value() ? *options.end_exclusive_utc - 1 : 0;
    if (!options.all_time) {
        summary.since = options.start_utc.has_value() ? iso8601_from_unix(*options.start_utc) : "";
        summary.until = options.end_exclusive_utc.has_value() ? iso8601_from_unix(*options.end_exclusive_utc - 1) : "";
    }

    bool used_rollups = false;
    double rollup_usd = 0.0;
    if (usage_window_has_bounded_range(options)) {
        const std::string start_date = utc_date_from_unix(*options.start_utc);
        const std::string end_date = utc_date_from_unix(*options.end_exclusive_utc - 1);
        const WindowRollupTotals totals =
            sum_user_request_totals(conn, user_id, options.token_id, start_date, end_date);
        apply_usage_totals_to_window_summary(summary, totals);
        rollup_usd = totals.usd;
        used_rollups = true;
        finalize_usage_window_rates(summary, options, window_start, window_end);
    } else {
        const std::string where = usage_where_clause(user_id, options, conn, true);
        const auto rows = conn.query_rows(
            "SELECT MIN(`time`),MAX(`time`),COUNT(*),"
            "COALESCE(SUM(COALESCE(input_tokens,0)+COALESCE(output_tokens,0)+"
            "COALESCE(cache_read_tokens,0)+COALESCE(cache_creation_5m_tokens,0)+COALESCE(cache_creation_1h_tokens,0)),0),"
            "COALESCE(SUM(COALESCE(input_tokens,0)),0),"
            "COALESCE(SUM(COALESCE(output_tokens,0)),0),"
            "COALESCE(SUM(COALESCE(cache_read_tokens,0)),0),"
            "COALESCE(SUM(COALESCE(cache_creation_5m_tokens,0)+COALESCE(cache_creation_1h_tokens,0)),0),"
            "COALESCE(SUM(CASE WHEN first_token_latency_ms>0 THEN first_token_latency_ms ELSE 0 END),0),"
            "COALESCE(SUM(CASE WHEN first_token_latency_ms>0 THEN 1 ELSE 0 END),0),"
            "COALESCE(SUM(COALESCE(output_tokens,0)),0),"
            "COALESCE(SUM(CASE WHEN latency_ms>first_token_latency_ms AND output_tokens IS NOT NULL "
            "THEN latency_ms-first_token_latency_ms ELSE 0 END),0) "
            "FROM requests" +
            where);
        if (!rows.empty()) {
            const MysqlResultRow &row = rows[0];
            summary.requests = parse_i64_or_zero(row.size() > 2 ? row[2] : std::nullopt);
            summary.tokens = parse_i64_or_zero(row.size() > 3 ? row[3] : std::nullopt);
            summary.input_tokens = parse_i64_or_zero(row.size() > 4 ? row[4] : std::nullopt);
            summary.output_tokens = parse_i64_or_zero(row.size() > 5 ? row[5] : std::nullopt);
            summary.cache_read_tokens = parse_i64_or_zero(row.size() > 6 ? row[6] : std::nullopt);
            summary.cache_creation_tokens = parse_i64_or_zero(row.size() > 7 ? row[7] : std::nullopt);
            const long long first_token_total = parse_i64_or_zero(row.size() > 8 ? row[8] : std::nullopt);
            summary.first_token_samples = parse_i64_or_zero(row.size() > 9 ? row[9] : std::nullopt);
            const long long output_for_tps = parse_i64_or_zero(row.size() > 10 ? row[10] : std::nullopt);
            const long long decode_latency_total = parse_i64_or_zero(row.size() > 11 ? row[11] : std::nullopt);
            if (summary.first_token_samples > 0) {
                summary.avg_first_token_latency =
                    static_cast<double>(first_token_total) / static_cast<double>(summary.first_token_samples);
            }
            if (decode_latency_total > 0 && output_for_tps > 0) {
                summary.tokens_per_second =
                    static_cast<double>(output_for_tps) * 1000.0 / static_cast<double>(decode_latency_total);
            }
            const long long cached = summary.cache_read_tokens + summary.cache_creation_tokens;
            if (summary.input_tokens > 0) {
                summary.cache_ratio = static_cast<double>(cached) / static_cast<double>(summary.input_tokens);
            }
            if (summary.since.empty() && row.size() > 0 && row[0].has_value()) {
                summary.since = mysql_datetime_to_iso8601_z(*row[0]);
                parse_mysql_datetime_utc(*row[0], window_start);
            }
            if (summary.until.empty() && row.size() > 1 && row[1].has_value()) {
                summary.until = mysql_datetime_to_iso8601_z(*row[1]);
                parse_mysql_datetime_utc(*row[1], window_end);
            }
            finalize_usage_window_rates(summary, options, window_start, window_end);
        }
    }

    if (used_rollups) {
        summary.committed_usd = decimal_to_string(rollup_usd);
        summary.used_usd = summary.committed_usd;
    } else {
        const std::string where = usage_where_clause(user_id, options, conn, true);
        const auto price_rows = conn.query_rows(
            "SELECT model,input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
            "output_tokens,tier_multiplier,channel_multiplier,service_tier "
            "FROM requests" +
            where);
        double committed = 0.0;
        for (const MysqlResultRow &row : price_rows) {
            Request req{ Model{}, 0, 0, 0, 0, 0 };
            req.model.name = row.size() > 0 ? trim_ascii(row[0].value_or("")) : "";
            req.input_tokens = static_cast<int>(parse_i64_or_zero(row.size() > 1 ? row[1] : std::nullopt));
            req.cache_read_tokens = static_cast<int>(parse_i64_or_zero(row.size() > 2 ? row[2] : std::nullopt));
            req.cache_creation_5m_tokens = static_cast<int>(parse_i64_or_zero(row.size() > 3 ? row[3] : std::nullopt));
            req.cache_creation_1h_tokens = static_cast<int>(parse_i64_or_zero(row.size() > 4 ? row[4] : std::nullopt));
            req.output_tokens = static_cast<int>(parse_i64_or_zero(row.size() > 5 ? row[5] : std::nullopt));
            const std::string tier_raw = row.size() > 6 ? trim_ascii(row[6].value_or("")) : "";
            req.tier_multiplier = tier_raw.empty() ? 1.0 : parse_decimal_or_zero(tier_raw);
            const std::string channel_raw = row.size() > 7 ? trim_ascii(row[7].value_or("")) : "";
            req.channel_multiplier = channel_raw.empty() ? 1.0 : parse_decimal_or_zero(channel_raw);
            req.service_tier = row.size() > 8 ? trim_ascii(row[8].value_or("")) : "";
            hydrate_request_model(req);
            committed += req.solve_price();
        }
        summary.committed_usd = decimal_to_string(committed);
        summary.used_usd = summary.committed_usd;
    }

    const auto balance = conn.query_one("SELECT usd FROM user_balances WHERE user_id=" + std::to_string(user_id));
    summary.limit_usd = balance.value_or("0");
    summary.remaining_usd = subtract_decimal(summary.limit_usd, summary.used_usd);
    return summary;
}

std::string usage_window_json(const UsageWindowSummary &summary)
{
    return "{\"window\":\"custom\",\"since\":\"" + json_escape(summary.since) + "\",\"until\":\"" +
           json_escape(summary.until) + "\",\"requests\":" + std::to_string(summary.requests) +
           ",\"tokens\":" + std::to_string(summary.tokens) + ",\"rpm\":" + std::to_string(summary.rpm) +
           ",\"tpm\":" + std::to_string(summary.tpm) + ",\"input_tokens\":" + std::to_string(summary.input_tokens) +
           ",\"output_tokens\":" + std::to_string(summary.output_tokens) +
           ",\"cache_read_tokens\":" + std::to_string(summary.cache_read_tokens) +
           ",\"cache_creation_tokens\":" + std::to_string(summary.cache_creation_tokens) +
           ",\"cache_ratio\":" + std::to_string(summary.cache_ratio) +
           ",\"first_token_samples\":" + std::to_string(summary.first_token_samples) +
           ",\"avg_first_token_latency\":" + std::to_string(summary.avg_first_token_latency) +
           ",\"tokens_per_second\":" + std::to_string(summary.tokens_per_second) + ",\"used_usd\":\"" +
           json_escape(summary.used_usd) + "\",\"committed_usd\":\"" + json_escape(summary.committed_usd) +
           "\",\"limit_usd\":\"" + json_escape(summary.limit_usd) + "\",\"remaining_usd\":\"" +
           json_escape(summary.remaining_usd) + "\"}";
}

std::string usage_series_point_json(const UsageSeriesPoint &point)
{
    return "{\"bucket\":\"" + json_escape(point.bucket) + "\",\"requests\":" + std::to_string(point.requests) +
           ",\"tokens\":" + std::to_string(point.tokens) + ",\"committed_usd\":" + std::to_string(point.committed_usd) +
           ",\"cache_ratio\":" + std::to_string(point.cache_ratio) +
           ",\"avg_first_token_latency\":" + std::to_string(point.avg_first_token_latency) +
           ",\"tokens_per_second\":" + std::to_string(point.tokens_per_second) + "}";
}

UserEventsPage query_requests(MysqlConnection &conn, long long user_id, const UsageQueryOptions &options,
                              const std::map<std::string, std::string> &params)
{
    int limit = 50;
    const std::string limit_raw = trim_ascii(query_param_value(params, "limit"));
    if (!limit_raw.empty()) {
        int parsed = 0;
        if (parse_i32(limit_raw, parsed) && parsed > 0 && parsed <= 100) {
            limit = parsed;
        }
    }
    std::optional<long long> before_id;
    const std::string before_id_raw = trim_ascii(query_param_value(params, "before_id"));
    if (!before_id_raw.empty()) {
        long long parsed = 0;
        if (parse_i64(before_id_raw, parsed) && parsed > 0) {
            before_id = parsed;
        }
    }
    const std::string q_model = trim_ascii(query_param_value(params, "q_model"));
    std::string start;
    std::string end_exclusive;
    if (!options.all_time) {
        if (options.start_utc.has_value()) {
            start = mysql_datetime_from_unix(*options.start_utc);
        }
        if (options.end_exclusive_utc.has_value()) {
            end_exclusive = mysql_datetime_from_unix(*options.end_exclusive_utc);
        }
    }

    UserEventsPage page;
    std::vector<Request> loaded;

    if (options.token_id.has_value() && !before_id.has_value() && q_model.empty()) {
        UserStore &users = UserStore::instance();
        users.reload(conn);
        loaded = users.tokens().requests(*options.token_id).list(start, end_exclusive, "", limit + 1);
    } else {
        std::string sql =
            "SELECT id,time,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
            "error_class,error_message,user_id,token_id,channel_id,status,model,service_tier,"
            "input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
            "output_tokens,tier_multiplier,channel_multiplier,is_stream FROM requests WHERE user_id=" +
            std::to_string(user_id);
        if (options.token_id.has_value()) {
            sql += " AND token_id=" + std::to_string(*options.token_id);
        }
        if (!start.empty()) {
            sql += " AND `time`>=" + conn.quote(start);
        }
        if (!end_exclusive.empty()) {
            sql += " AND `time`<" + conn.quote(end_exclusive);
        }
        if (!q_model.empty()) {
            sql += " AND model LIKE " + conn.quote("%" + q_model + "%");
        }
        if (before_id.has_value()) {
            sql += " AND id<" + std::to_string(*before_id);
        }
        sql += " ORDER BY id DESC LIMIT " + std::to_string(limit + 1);
        const auto rows = conn.query_rows(sql);
        loaded.reserve(rows.size());
        for (const auto &row : rows) {
            Request req = row_to_request(row);
            hydrate_request_model(req);
            loaded.push_back(std::move(req));
        }
    }

    const bool has_extra = static_cast<int>(loaded.size()) > limit;
    if (has_extra) {
        loaded.resize(static_cast<size_t>(limit));
    }
    page.events.reserve(loaded.size());
    for (const Request &req : loaded) {
        page.events.push_back(request_to_user_event(req));
    }
    if (has_extra && !page.events.empty()) {
        page.next_before_id = page.events.back().id;
    }
    return page;
}

std::vector<UsageSeriesPoint> query_usage_time_series(MysqlConnection &conn, long long user_id,
                                                      const UsageQueryOptions &options, std::string_view granularity)
{
    const std::string where = usage_where_clause(user_id, options, conn);
    const auto rows = conn.query_rows(
        "SELECT `time`,status,input_tokens,output_tokens,cache_read_tokens,"
        "cache_creation_5m_tokens,cache_creation_1h_tokens,first_token_latency_ms,latency_ms,"
        "model,tier_multiplier,channel_multiplier,service_tier "
        "FROM requests" +
        where + " ORDER BY `time` ASC, id ASC");
    struct BucketAccumulator {
        long long requests = 0;
        long long tokens = 0;
        double committed_usd = 0.0;
        long long input = 0;
        long long cached = 0;
        long long first_token_total = 0;
        long long first_token_count = 0;
        long long output = 0;
        long long latency_total = 0;
    };
    std::map<std::string, BucketAccumulator> buckets;
    for (const MysqlResultRow &row : rows) {
        if (row.size() < 13 || !row[0].has_value()) {
            continue;
        }
        time_t event_utc = 0;
        if (!parse_mysql_datetime_utc(*row[0], event_utc)) {
            continue;
        }
        const std::string bucket = granularity == "day" ? iso8601_day_bucket_from_utc(event_utc, options.time_zone) :
                                                          iso8601_hour_bucket_from_utc(event_utc, options.time_zone);
        BucketAccumulator &acc = buckets[bucket];
        acc.requests += 1;
        const long long input = parse_i64_or_zero(row[2]);
        const long long output = parse_i64_or_zero(row[3]);
        const long long cache_read = parse_i64_or_zero(row[4]);
        const long long cache_creation =
            parse_i64_or_zero(row[5]) + parse_i64_or_zero(row[6]);
        acc.tokens += input + output + cache_read + cache_creation;
        if (row[1].has_value() && *row[1] == "committed") {
            Request req{ Model{}, 0, 0, 0, 0, 0 };
            req.model.name = trim_ascii(row[9].value_or(""));
            req.input_tokens = static_cast<int>(input);
            req.output_tokens = static_cast<int>(output);
            req.cache_read_tokens = static_cast<int>(cache_read);
            req.cache_creation_5m_tokens = static_cast<int>(parse_i64_or_zero(row[5]));
            req.cache_creation_1h_tokens = static_cast<int>(parse_i64_or_zero(row[6]));
            const std::string tier_raw = trim_ascii(row[10].value_or(""));
            req.tier_multiplier = tier_raw.empty() ? 1.0 : parse_decimal_or_zero(tier_raw);
            const std::string channel_raw = trim_ascii(row[11].value_or(""));
            req.channel_multiplier = channel_raw.empty() ? 1.0 : parse_decimal_or_zero(channel_raw);
            req.service_tier = trim_ascii(row[12].value_or(""));
            hydrate_request_model(req);
            acc.committed_usd += req.solve_price();
        }
        acc.input += input;
        acc.cached += cache_read + cache_creation;
        const long long first_token_latency = parse_i64_or_zero(row[7]);
        if (first_token_latency > 0) {
            acc.first_token_total += first_token_latency;
            acc.first_token_count += 1;
        }
        const long long latency_ms = parse_i64_or_zero(row[8]);
        if (latency_ms > 0) {
            acc.output += output;
            acc.latency_total += latency_ms;
        }
    }

    std::vector<UsageSeriesPoint> points;
    points.reserve(buckets.size());
    for (const auto &entry : buckets) {
        UsageSeriesPoint point;
        point.bucket = entry.first;
        point.requests = entry.second.requests;
        point.tokens = entry.second.tokens;
        point.committed_usd = entry.second.committed_usd;
        point.cache_ratio = entry.second.input > 0 ?
                                static_cast<double>(entry.second.cached) / static_cast<double>(entry.second.input) :
                                0.0;
        point.avg_first_token_latency = entry.second.first_token_count > 0 ?
                                            static_cast<double>(entry.second.first_token_total) /
                                                static_cast<double>(entry.second.first_token_count) :
                                            0.0;
        point.tokens_per_second = entry.second.latency_total > 0 ? static_cast<double>(entry.second.output) * 1000.0 /
                                                                       static_cast<double>(entry.second.latency_total) :
                                                                   0.0;
        points.push_back(std::move(point));
    }
    return points;
}

std::string dashboard_model_color(size_t index)
{
    static constexpr const char *kColors[] = { "#3b82f6", "#22c55e", "#f59e0b", "#ef4444", "#8b5cf6", "#06b6d4" };
    return kColors[index % (sizeof(kColors) / sizeof(kColors[0]))];
}

std::vector<DashboardModelStat> query_dashboard_model_stats(MysqlConnection &conn, long long user_id,
                                                            const UsageQueryOptions &options)
{
    const std::string where = usage_where_clause(user_id, options, conn);
    const auto model_rows = conn.query_rows("SELECT COALESCE(model,''),COUNT(*),"
                                            "COALESCE(SUM(COALESCE(input_tokens,0)+COALESCE(output_tokens,0)+"
                                            "COALESCE(cache_read_tokens,0)+COALESCE(cache_creation_5m_tokens,0)+"
                                            "COALESCE(cache_creation_1h_tokens,0)),0) "
                                            "FROM requests" +
                                            where + " GROUP BY model ORDER BY 2 DESC LIMIT 12");
    std::vector<DashboardModelStat> stats;
    stats.reserve(model_rows.size());
    for (size_t i = 0; i < model_rows.size(); ++i) {
        const MysqlResultRow &row = model_rows[i];
        DashboardModelStat stat;
        stat.model = row.size() > 0 && row[0].has_value() ? *row[0] : "";
        stat.requests = parse_i64_or_zero(row.size() > 1 ? row[1] : std::nullopt);
        stat.tokens = parse_i64_or_zero(row.size() > 2 ? row[2] : std::nullopt);
        double committed = 0.0;
        const std::string price_where = where + " AND COALESCE(model,'')=" + conn.quote(stat.model);
        const auto price_rows = conn.query_rows(
            "SELECT model,input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
            "output_tokens,tier_multiplier,channel_multiplier FROM requests" +
            price_where);
        for (const MysqlResultRow &prow : price_rows) {
            Request req{ Model{}, 0, 0, 0, 0, 0 };
            req.model.name = prow.size() > 0 ? trim_ascii(prow[0].value_or("")) : "";
            req.input_tokens = static_cast<int>(parse_i64_or_zero(prow.size() > 1 ? prow[1] : std::nullopt));
            req.cache_read_tokens = static_cast<int>(parse_i64_or_zero(prow.size() > 2 ? prow[2] : std::nullopt));
            req.cache_creation_5m_tokens =
                static_cast<int>(parse_i64_or_zero(prow.size() > 3 ? prow[3] : std::nullopt));
            req.cache_creation_1h_tokens =
                static_cast<int>(parse_i64_or_zero(prow.size() > 4 ? prow[4] : std::nullopt));
            req.output_tokens = static_cast<int>(parse_i64_or_zero(prow.size() > 5 ? prow[5] : std::nullopt));
            const std::string tier_raw = prow.size() > 6 ? trim_ascii(prow[6].value_or("")) : "";
            req.tier_multiplier = tier_raw.empty() ? 1.0 : parse_decimal_or_zero(tier_raw);
            const std::string channel_raw = prow.size() > 7 ? trim_ascii(prow[7].value_or("")) : "";
            req.channel_multiplier = channel_raw.empty() ? 1.0 : parse_decimal_or_zero(channel_raw);
            hydrate_request_model(req);
            committed += req.solve_price();
        }
        stat.committed_usd = decimal_to_string(committed);
        const std::vector<Model> &models = ModelManager::instance().models();
        const auto model_it = std::ranges::find(models, stat.model, &Model::name);
        stat.icon_url = model_it != models.end() ? model_icon_url(model_it->owned_by) : "";
        stat.color = dashboard_model_color(i);
        stats.push_back(std::move(stat));
    }
    return stats;
}

std::string dashboard_model_stats_json(const std::vector<DashboardModelStat> &stats)
{
    std::string json = "[";
    for (size_t i = 0; i < stats.size(); ++i) {
        if (i > 0) {
            json += ",";
        }
        const DashboardModelStat &stat = stats[i];
        json += "{\"model\":\"" + json_escape(stat.model) +
                "\",\"icon_url\":" + (stat.icon_url.empty() ? "null" : "\"" + json_escape(stat.icon_url) + "\"") +
                ",\"color\":\"" + json_escape(stat.color) + "\",\"requests\":" + std::to_string(stat.requests) +
                ",\"tokens\":" + std::to_string(stat.tokens) + ",\"committed_usd\":\"" +
                json_escape(stat.committed_usd) + "\"}";
    }
    json += "]";
    return json;
}

HttpResponse user_models_detail_response(std::string_view raw_request, const Config &config,
                                         std::string_view request_id)
{
    HttpResponse auth_response;
    const auto user = require_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    try {
        const std::vector<Model> &models = ModelManager::instance().models();
        return api_json_response(api_success(user_models_json(models)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse dashboard_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                const ParsedTarget &parsed)
{
    HttpResponse auth_response;
    const auto user = require_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }

    const std::map<std::string, std::string> params = parse_query_map(parsed.target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return api_json_response(api_failure(message), request_id);
    }

    const time_t now = std::time(nullptr);
    const std::tm tm = local_tm_from_utc(now, options.time_zone);
    const int year = tm.tm_year + 1900;
    const int month = tm.tm_mon + 1;
    const int day = tm.tm_mday;
    options.all_time = false;
    options.start_utc = utc_seconds_from_local_date(year, month, day, options.time_zone);
    int next_year = year;
    int next_month = month;
    int next_day = day;
    next_date(next_year, next_month, next_day);
    options.end_exclusive_utc = utc_seconds_from_local_date(next_year, next_month, next_day, options.time_zone);

    try {
        MysqlConnection conn(config.db_dsn);
        const UsageWindowSummary today = query_usage_window_summary(conn, user->id, options);
        std::vector<UsageSeriesPoint> points = query_usage_time_series(conn, user->id, options, "hour");
        const std::vector<DashboardModelStat> model_stats = query_dashboard_model_stats(conn, user->id, options);
        std::string points_json = "[";
        for (size_t i = 0; i < points.size(); ++i) {
            if (i > 0)
                points_json += ",";
            points_json += usage_series_point_json(points[i]);
        }
        points_json += "]";
        const std::string body = "{\"today_usage_usd\":\"" + json_escape(today.committed_usd) +
                                 "\",\"today_since\":\"" + json_escape(today.since) + "\",\"today_until\":\"" +
                                 json_escape(today.until) + "\",\"today_requests\":" + std::to_string(today.requests) +
                                 ",\"today_tokens\":" + std::to_string(today.tokens) + ",\"today_rpm\":\"" +
                                 std::to_string(today.rpm) + "\",\"today_tpm\":\"" + std::to_string(today.tpm) +
                                 "\",\"charts\":{\"model_stats\":" + dashboard_model_stats_json(model_stats) +
                                 ",\"time_series_stats\":" + points_json + "}}";
        return api_json_response(api_success(body), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse usage_windows_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                    const ParsedTarget &parsed)
{
    HttpResponse auth_response;
    const auto user = require_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    const std::map<std::string, std::string> params = parse_query_map(parsed.target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return api_json_response(api_failure(message), request_id);
    }
    try {
        MysqlConnection conn(config.db_dsn);
        const UsageWindowSummary summary = query_usage_window_summary(conn, user->id, options);
        const std::string body = "{\"time_zone\":\"" + json_escape(options.time_zone) + "\",\"now\":\"" +
                                 json_escape(iso8601_from_unix(std::time(nullptr))) + "\",\"windows\":[" +
                                 usage_window_json(summary) + "]}";
        return api_json_response(api_success(body), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse requests_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                   const ParsedTarget &parsed)
{
    HttpResponse auth_response;
    const auto user = require_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    const std::map<std::string, std::string> params = parse_query_map(parsed.target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return api_json_response(api_failure(message), request_id);
    }
    try {
        MysqlConnection conn(config.db_dsn);
        const UserEventsPage page = query_requests(conn, user->id, options, params);
        return api_json_response(api_success(requests_page_to_user_json(page)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse usage_timeseries_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                       const ParsedTarget &parsed)
{
    HttpResponse auth_response;
    const auto user = require_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    const std::map<std::string, std::string> params = parse_query_map(parsed.target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return api_json_response(api_failure(message), request_id);
    }
    std::string granularity = trim_ascii(query_param_value(params, "granularity"));
    if (granularity.empty()) {
        granularity = "day";
    }
    if (granularity != "hour" && granularity != "day") {
        return api_json_response(api_failure("granularity 无效"), request_id);
    }
    try {
        MysqlConnection conn(config.db_dsn);
        const std::vector<UsageSeriesPoint> points = query_usage_time_series(conn, user->id, options, granularity);
        std::string points_json = "[";
        for (size_t i = 0; i < points.size(); ++i) {
            if (i > 0)
                points_json += ",";
            points_json += usage_series_point_json(points[i]);
        }
        points_json += "]";
        const std::string body =
            "{\"time_zone\":\"" + json_escape(options.time_zone) + "\",\"start\":\"" +
            json_escape(options.start_utc.has_value() ? iso8601_from_unix(*options.start_utc) : "") + "\",\"end\":\"" +
            json_escape(options.end_exclusive_utc.has_value() ? iso8601_from_unix(*options.end_exclusive_utc - 1) :
                                                                "") +
            "\",\"granularity\":\"" + json_escape(granularity) + "\",\"points\":" + points_json + "}";
        return api_json_response(api_success(body), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse usage_event_detail_response(std::string_view raw_request, const Config &config,
                                         std::string_view request_id, long long event_id)
{
    HttpResponse auth_response;
    const auto user = require_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    if (event_id <= 0) {
        return api_json_response(api_failure("event_id 无效"), request_id);
    }
    try {
        MysqlConnection conn(config.db_dsn);
        const auto token_row = conn.query_one(
            "SELECT token_id FROM requests WHERE id=" + std::to_string(event_id) + " AND user_id=" +
            std::to_string(user->id) + " LIMIT 1");
        if (!token_row.has_value()) {
            return api_json_response(api_failure("事件不存在"), request_id);
        }
        long long token_id = 0;
        if (!parse_i64(*token_row, token_id) || token_id <= 0) {
            return api_json_response(api_failure("事件不存在"), request_id);
        }
        UserStore &users = UserStore::instance();
        users.reload(conn);
        const std::optional<Request> req = users.tokens().requests(token_id).get(event_id);
        if (!req.has_value()) {
            return api_json_response(api_failure("事件不存在"), request_id);
        }
        const auto detail = request_to_user_event_detail(*req);
        if (!detail.has_value()) {
            return api_json_response(api_failure("事件不存在"), request_id);
        }
        return api_json_response(api_success(usage_event_detail_to_json(*detail)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

} // namespace

HttpResponse user_models_detail_http_response(std::string_view raw_request, const Config &config,
                                              std::string_view request_id)
{
    return user_models_detail_response(raw_request, config, request_id);
}
HttpResponse dashboard_http_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                     std::string_view target)
{
    ParsedTarget parsed{ target };
    return dashboard_response(raw_request, config, request_id, parsed);
}
HttpResponse usage_windows_http_response(std::string_view raw_request, const Config &config,
                                         std::string_view request_id, std::string_view target)
{
    ParsedTarget parsed{ target };
    return usage_windows_response(raw_request, config, request_id, parsed);
}
HttpResponse requests_http_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                        std::string_view target)
{
    ParsedTarget parsed{ target };
    return requests_response(raw_request, config, request_id, parsed);
}
HttpResponse usage_timeseries_http_response(std::string_view raw_request, const Config &config,
                                            std::string_view request_id, std::string_view target)
{
    ParsedTarget parsed{ target };
    return usage_timeseries_response(raw_request, config, request_id, parsed);
}
HttpResponse usage_event_detail_http_response(std::string_view raw_request, const Config &config,
                                              std::string_view request_id, long long event_id)
{
    return usage_event_detail_response(raw_request, config, request_id, event_id);
}

} // namespace revlm
