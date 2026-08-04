#include "server/http_dispatch.hpp"
#include "server/http_server.hpp"
#include "auth/security.hpp"
#include "users/users.hpp"
#include "users/user_api.hpp"
#include "users/user_admin_api.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "config/config.hpp"
#include "models/catalog.hpp"
#include "plugins/packages.hpp"
#include "proxy/gateway.hpp"
#include "request/request.hpp"
#include "users/token_api.hpp"
#include "store/database.hpp"
#include "util/datetime.hpp"
#include "request/proxy_request.hpp"
#include "util/http_query.hpp"
#include "util/json.hpp"
#include "util/json_convert.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"
#include "util/user_input.hpp"
#include "revlm_entities-odb.hxx"

#include <cstdint>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <date/date.h>
#include <date/tz.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <odb/mysql/query.hxx>
#include <odb/nullable.hxx>
#include <odb/query.hxx>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revlm
{
namespace
{

struct ParsedRequest {
    std::string_view method;
    std::string_view path;
    std::string_view target;
    size_t header_bytes = 0;
    size_t content_length = 0;
    bool invalid_framing = false;
};

struct RequestContext {
    ParsedRequest parsed;
    std::string raw_request;
    std::string usage_event_id;
    std::string client_ip;
    std::string set_cookie;
};

std::string serialize_json_http_bytes(int status, std::string_view reason, const json &body)
{
    const std::string payload = serialize(body);
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
        << "Content-Type: application/json; charset=utf-8\r\n"
        << "Content-Length: " << payload.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n";
    std::string bytes = out.str();
    bytes.append(payload);
    return bytes;
}

std::string build_raw_http_request(const ::httplib::Request &req)
{
    std::ostringstream out;
    out << req.method << ' ' << req.target << " HTTP/1.1\r\n";
    for (const auto &header : req.headers) {
        out << header.first << ": " << header.second << "\r\n";
    }
    out << "\r\n" << req.body;
    return out.str();
}

// Correlation id, not a client contract: upstreams return it in the response, never require it in.
// Honor a client-supplied id (X-Request-Id, then legacy x-client-request-id); otherwise mint one server-side
// so it is always present for logging/persistence. Oversized ids are untrusted -> replaced, never rejected.
std::string resolve_request_id(const ::httplib::Request &req)
{
    std::string id = trim_ascii(req.get_header_value("X-Request-Id"));
    if (id.empty())
        id = trim_ascii(req.get_header_value("x-client-request-id"));
    return (id.empty() || id.size() > 128) ? "req_" + boost::uuids::to_string(boost::uuids::random_generator{}()) : id;
}

json billing_balance_response(std::string_view raw_request, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    try {
        UserStore &store = UserStore::instance();
        return json(
            { { "success", true }, { "data", json{ { "balance_usd", store.get_user_balance_usd(user->id) } } } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

ParsedRequest parsed_request_from_httplib(const ::httplib::Request &req)
{
    ParsedRequest parsed;
    parsed.method = req.method;
    parsed.target = req.target;
    parsed.path = req.path;
    parsed.content_length = req.body.size();
    size_t header_bytes = req.method.size() + req.target.size() + 12;
    for (const auto &header : req.headers) {
        header_bytes += header.first.size() + header.second.size() + 4;
    }
    parsed.header_bytes = header_bytes + 4;
    return parsed;
}

bool validate_parsed_request(const ParsedRequest &parsed, ::httplib::Response &res)
{
    if (parsed.header_bytes > static_cast<size_t>(config().http_max_header_bytes)) {
        write_json(res, 431, json("request header too large"));
        return false;
    }
    const size_t body_limit = parsed.path == "/api/admin/plugins/upload" ?
                                  static_cast<size_t>(config().plugin_max_archive_bytes) :
                                  static_cast<size_t>(config().http_max_body_bytes);
    if (parsed.content_length > body_limit) {
        write_json(res, 413, json("payload too large"));
        return false;
    }
    return true;
}

RequestContext make_request_context(const ::httplib::Request &req)
{
    const std::string client_ip = req.remote_addr.empty() ? "127.0.0.1" : req.remote_addr;
    return RequestContext{
        .parsed = parsed_request_from_httplib(req),
        .raw_request = inject_request_metadata(build_raw_http_request(req), client_ip),
        .usage_event_id = "req_" + boost::uuids::to_string(boost::uuids::random_generator{}()),
        .client_ip = client_ip,
    };
}

void log_access(::httplib::Response &res, std::string_view method, std::string_view path, int status)
{
    const std::string request_id = res.get_header_value("X-Request-Id");
    std::cerr << "access request_id=" << request_id << " status=" << status << " method=" << method
              << " path=" << redact_request_target(path) << '\n';
}

::httplib::Server::Handler
make_http_handler(std::function<void(const ::httplib::Request &, ::httplib::Response &, RequestContext &)> handler)
{
    return [handler = std::move(handler)](const ::httplib::Request &req, ::httplib::Response &res) {
        RequestContext ctx = make_request_context(req);
        res.set_header("X-Request-Id", resolve_request_id(req));
        if (!validate_parsed_request(ctx.parsed, res)) {
            log_access(res, ctx.parsed.method, ctx.parsed.target, res.status);
            return;
        }
        handler(req, res, ctx);
        log_access(res, ctx.parsed.method, ctx.parsed.target, res.status);
    };
}

::httplib::Server::Handler
make_response_handler(std::function<json(const ::httplib::Request &, RequestContext &)> handler)
{
    return make_http_handler(
        [handler = std::move(handler)](const ::httplib::Request &req, ::httplib::Response &res, RequestContext &ctx) {
            write_json(res, 200, handler(req, ctx), ctx.set_cookie);
        });
}

std::optional<long long> path_param_i64(const ::httplib::Request &req, std::string_view name)
{
    const auto it = req.path_params.find(std::string{ name });
    if (it == req.path_params.end()) {
        return std::nullopt;
    }
    return parse_positive_i64_or(it->second);
}

class InMemoryHttpServer final : public ::httplib::Server {
public:
    bool process(::httplib::Stream &stream, const std::function<void(::httplib::Request &)> &setup_request)
    {
        bool connection_closed = false;
        // Ubuntu 24.04 ships cpp-httplib 0.14.3 (4-arg). 0.25+ define VERSION_NUM and use addr args.
#ifdef CPPHTTPLIB_VERSION_NUM
        return process_request(stream, "127.0.0.1", 0, "127.0.0.1", 0, true, connection_closed, setup_request);
#else
        return process_request(stream, true, connection_closed, setup_request);
#endif
    }
};

// Usage / request analytics handlers (merged from former usage/*_api.cpp).
constexpr std::string_view kAdminTimeZone = "Asia/Shanghai";

struct UsageQueryOptions {
    std::string time_zone = "UTC";
    bool all_time = false;
    std::optional<sys_seconds> start_utc;
    std::optional<sys_seconds> end_exclusive_utc;
    std::optional<long long> token_id;
};

std::optional<std::string> nullable_odb_string(const odb::nullable<std::string> &value)
{
    if (value.null() || value->empty()) {
        return std::nullopt;
    }
    return *value;
}

bool parse_usage_query_options(const std::map<std::string, std::string> &params, UsageQueryOptions &out,
                               std::string &message)
{
    out = UsageQueryOptions{};
    out.time_zone = trim_ascii(query_param_value(params, "tz"));
    if (out.time_zone.empty()) {
        out.time_zone = "UTC";
    }
    if (!zone_exists(out.time_zone)) {
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
    if (!start.empty()) {
        int y = 0;
        int m = 0;
        int d = 0;
        if (!parse_date_yyyy_mm_dd(start, y, m, d)) {
            message = "start 无效";
            return false;
        }
        out.start_utc = local_date_to_utc(y, static_cast<unsigned>(m), static_cast<unsigned>(d), out.time_zone);
    }
    if (!end.empty()) {
        int y = 0;
        int m = 0;
        int d = 0;
        if (!parse_date_yyyy_mm_dd(end, y, m, d)) {
            message = "end 无效";
            return false;
        }
        unsigned um = static_cast<unsigned>(m);
        unsigned ud = static_cast<unsigned>(d);
        const sys_seconds end_start = local_date_to_utc(y, um, ud, out.time_zone);
        next_date(y, um, ud);
        out.end_exclusive_utc = local_date_to_utc(y, um, ud, out.time_zone);
        if (*out.end_exclusive_utc <= end_start) {
            out.end_exclusive_utc = end_start + std::chrono::seconds{ 86400 };
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

RequestListFilter filter_from_usage_options(long long user_id, const UsageQueryOptions &options)
{
    RequestListFilter filter;
    filter.user_id = user_id;
    if (options.token_id.has_value()) {
        filter.token_id = options.token_id;
    }
    if (!options.all_time) {
        if (options.start_utc.has_value()) {
            filter.start = to_mysql_datetime(*options.start_utc);
        }
        if (options.end_exclusive_utc.has_value()) {
            filter.end_exclusive = to_mysql_datetime(*options.end_exclusive_utc);
        }
    }
    return filter;
}

json request_to_user_event_json(const Request &req)
{
    json o = to_json(req);
    o["time"] = req.time.empty() ? std::string{} : to_iso8601z(parse_mysql_datetime(req.time));
    o["response_id"] = req.response_id.null() ? json(nullptr) : json(*req.response_id);
    o["channel_id"] = req.channel_id > 0 ? json(req.channel_id) : json(nullptr);
    o["model"] = req.model_name.null() || req.model_name->empty() ? json(nullptr) : json(*req.model_name);
    const revlm::UsageTokens tokens = revlm::usage_tokens(req);
    o["cache_creation_tokens"] = tokens.cache_creation_5m_tokens + tokens.cache_creation_1h_tokens;
    o["cost_usd"] = request_detail::decimal_to_string(req.solve_price());
    return o;
}

json aggregate_window(const std::vector<Request> &rows, const UsageQueryOptions &options)
{
    long long requests = 0;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_tokens = 0;
    long long cache_creation_tokens = 0;
    long long first_token_sum = 0;
    long long first_token_samples = 0;
    long long decode_tokens = 0;
    long long decode_latency_ms = 0;
    double used = 0.0;
    std::optional<sys_seconds> min_time;
    std::optional<sys_seconds> max_time;

    for (const Request &req : rows) {
        const revlm::UsageTokens tokens = revlm::usage_tokens(req);
        ++requests;
        input_tokens += tokens.input_tokens;
        output_tokens += tokens.output_tokens;
        cache_read_tokens += tokens.cache_read_tokens;
        cache_creation_tokens += tokens.cache_creation_5m_tokens + tokens.cache_creation_1h_tokens;
        used += req.solve_price();
        if (req.first_token_latency_ms > 0) {
            first_token_sum += req.first_token_latency_ms;
            ++first_token_samples;
        }
        if (req.latency_ms > req.first_token_latency_ms && tokens.output_tokens > 0) {
            decode_tokens += tokens.output_tokens;
            decode_latency_ms += req.latency_ms - req.first_token_latency_ms;
        }
        if (!req.time.empty()) {
            try {
                const sys_seconds tp = parse_mysql_datetime(req.time);
                if (!min_time.has_value() || tp < *min_time) {
                    min_time = tp;
                }
                if (!max_time.has_value() || tp > *max_time) {
                    max_time = tp;
                }
            } catch (const std::exception &) {
            }
        }
    }

    const long long tokens = input_tokens + output_tokens + cache_read_tokens + cache_creation_tokens;
    std::string since;
    std::string until;
    if (!options.all_time) {
        if (options.start_utc.has_value()) {
            since = to_iso8601z(*options.start_utc);
        }
        if (options.end_exclusive_utc.has_value()) {
            until = to_iso8601z(*options.end_exclusive_utc - std::chrono::seconds{ 1 });
        }
    }
    if (since.empty() && min_time.has_value()) {
        since = to_iso8601z(*min_time);
    }
    if (until.empty() && max_time.has_value()) {
        until = to_iso8601z(*max_time);
    }

    double minutes = 1.0;
    if (options.start_utc.has_value() && options.end_exclusive_utc.has_value()) {
        minutes = std::max(1.0, std::chrono::duration<double>(*options.end_exclusive_utc - *options.start_utc).count() /
                                    60.0);
    } else if (min_time.has_value() && max_time.has_value() && *max_time >= *min_time) {
        minutes = std::max(1.0, std::chrono::duration<double>(*max_time - *min_time).count() / 60.0 + 1.0 / 60.0);
    }

    json window;
    window["window"] = "custom";
    window["since"] = since;
    window["until"] = until;
    window["requests"] = requests;
    window["tokens"] = tokens;
    window["rpm"] = static_cast<long long>(std::llround(static_cast<double>(requests) / minutes));
    window["tpm"] = static_cast<long long>(std::llround(static_cast<double>(tokens) / minutes));
    window["input_tokens"] = input_tokens;
    window["output_tokens"] = output_tokens;
    window["cache_read_tokens"] = cache_read_tokens;
    window["cache_creation_tokens"] = cache_creation_tokens;
    window["cache_ratio"] = input_tokens > 0 ? static_cast<double>(cache_read_tokens + cache_creation_tokens) /
                                                   static_cast<double>(input_tokens) :
                                               0.0;
    window["first_token_samples"] = first_token_samples;
    window["avg_first_token_latency"] =
        first_token_samples > 0 ? static_cast<double>(first_token_sum) / static_cast<double>(first_token_samples) : 0.0;
    window["tokens_per_second"] =
        decode_latency_ms > 0 ? static_cast<double>(decode_tokens) * 1000.0 / static_cast<double>(decode_latency_ms) :
                                0.0;
    window["usd"] = request_detail::decimal_to_string(used);
    return window;
}

json usage_time_series(const std::vector<Request> &rows, const std::string &tz, std::string_view granularity)
{
    struct Bucket {
        long long requests = 0;
        long long input_tokens = 0;
        long long output_tokens = 0;
        long long cache_read_tokens = 0;
        long long cache_creation_tokens = 0;
        long long tokens = 0;
        long long first_token_latency_sum = 0;
        double usd = 0.0;
    };
    std::map<std::string, Bucket> buckets;
    for (const Request &req : rows) {
        if (req.time.empty()) {
            continue;
        }
        sys_seconds tp;
        try {
            tp = parse_mysql_datetime(req.time);
        } catch (const std::exception &) {
            continue;
        }
        const std::string bucket = granularity == "day" ? day_bucket(tp, tz) : hour_bucket(tp, tz);
        Bucket &total = buckets[bucket];
        const revlm::UsageTokens tokens = revlm::usage_tokens(req);
        const long long cache_creation = tokens.cache_creation_5m_tokens + tokens.cache_creation_1h_tokens;
        ++total.requests;
        total.input_tokens += tokens.input_tokens;
        total.output_tokens += tokens.output_tokens;
        total.cache_read_tokens += tokens.cache_read_tokens;
        total.cache_creation_tokens += cache_creation;
        total.tokens += tokens.input_tokens + tokens.output_tokens + tokens.cache_read_tokens + cache_creation;
        total.usd += req.solve_price();
        total.first_token_latency_sum += std::max(req.first_token_latency_ms, 0);
    }

    json points = json::array();
    for (const auto &[bucket, total] : buckets) {
        const long long cached = total.cache_read_tokens + total.cache_creation_tokens;
        json point;
        point["bucket"] = bucket;
        point["requests"] = total.requests;
        point["tokens"] = total.tokens;
        point["usd"] = total.usd;
        point["cache_ratio"] =
            total.input_tokens > 0 ? static_cast<double>(cached) / static_cast<double>(total.input_tokens) : 0.0;
        point["avg_first_token_latency"] = total.requests > 0 ? static_cast<double>(total.first_token_latency_sum) /
                                                                    static_cast<double>(total.requests) :
                                                                0.0;
        point["tokens_per_second"] = 0.0;
        points.push_back(std::move(point));
    }
    return points;
}

json dashboard_model_stats(const std::vector<Request> &rows)
{
    struct ModelStats {
        long long requests = 0;
        long long tokens = 0;
        double usd = 0.0;
    };
    std::map<std::string, ModelStats> by_model;
    for (const Request &req : rows) {
        const std::string model = req.model_name.null() ? "" : *req.model_name;
        ModelStats &total = by_model[model];
        const revlm::UsageTokens tokens = revlm::usage_tokens(req);
        ++total.requests;
        total.tokens += tokens.input_tokens + tokens.output_tokens + tokens.cache_read_tokens +
                        tokens.cache_creation_5m_tokens + tokens.cache_creation_1h_tokens;
        total.usd += req.solve_price();
    }
    std::vector<std::pair<std::string, ModelStats>> ranked(by_model.begin(), by_model.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        if (a.second.requests != b.second.requests) {
            return a.second.requests > b.second.requests;
        }
        return a.first < b.first;
    });
    if (ranked.size() > 12) {
        ranked.resize(12);
    }
    static constexpr const char *kColors[] = { "#3b82f6", "#22c55e", "#f59e0b", "#ef4444", "#8b5cf6", "#06b6d4" };
    json out = json::array();
    for (size_t i = 0; i < ranked.size(); ++i) {
        json o;
        o["model"] = ranked[i].first;
        o["icon_url"] = nullptr;
        o["color"] = kColors[i % (sizeof(kColors) / sizeof(kColors[0]))];
        o["requests"] = ranked[i].second.requests;
        o["tokens"] = ranked[i].second.tokens;
        o["usd"] = request_detail::decimal_to_string(ranked[i].second.usd);
        out.push_back(std::move(o));
    }
    return out;
}

json user_models_detail_http_response(std::string_view raw_request, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    json models_json = json::array();
    for (const Model &model : all_known_models()) {
        json o;
        o["id"] = model.id;
        o["public_id"] = model.name;
        o["pricing"] = model.pricing.is_object() ? model.pricing : json{};
        models_json.push_back(std::move(o));
    }
    return json({ { "success", true }, { "data", std::move(models_json) } });
}

json dashboard_http_response(std::string_view raw_request, std::string_view target, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return json({ { "success", false }, { "message", message } });
    }

    const auto now = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto local = date::make_zoned(options.time_zone, now).get_local_time();
    const date::year_month_day ymd{ date::floor<date::days>(local) };
    int year = static_cast<int>(ymd.year());
    unsigned month = static_cast<unsigned>(ymd.month());
    unsigned day = static_cast<unsigned>(ymd.day());
    options.all_time = false;
    options.start_utc = local_date_to_utc(year, month, day, options.time_zone);
    next_date(year, month, day);
    options.end_exclusive_utc = local_date_to_utc(year, month, day, options.time_zone);

    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        const json today = aggregate_window(rows, options);
        json charts;
        charts["model_stats"] = dashboard_model_stats(rows);
        charts["time_series_stats"] = usage_time_series(rows, options.time_zone, "hour");
        json body;
        body["today_usage_usd"] = today["usd"];
        body["today_since"] = today["since"];
        body["today_until"] = today["until"];
        body["today_requests"] = today["requests"];
        body["today_tokens"] = today["tokens"];
        body["today_rpm"] = std::to_string(today["rpm"].as_int64().value_or(0));
        body["today_tpm"] = std::to_string(today["tpm"].as_int64().value_or(0));
        body["charts"] = std::move(charts);
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json usage_windows_http_response(std::string_view raw_request, std::string_view target, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return json({ { "success", false }, { "message", message } });
    }
    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        json body;
        body["time_zone"] = options.time_zone;
        body["now"] = to_iso8601z(date::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        json windows;
        windows.push_back(aggregate_window(rows, options));
        body["windows"] = std::move(windows);
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json requests_http_response(std::string_view raw_request, std::string_view target, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return json({ { "success", false }, { "message", message } });
    }

    int limit = 50;
    const std::string limit_raw = trim_ascii(query_param_value(params, "limit"));
    if (!limit_raw.empty()) {
        int parsed = 0;
        if (parse_i32(limit_raw, parsed) && parsed > 0 && parsed <= 100) {
            limit = parsed;
        }
    }
    RequestListFilter filter = filter_from_usage_options(user->id, options);
    const std::string before_id_raw = trim_ascii(query_param_value(params, "before_id"));
    if (!before_id_raw.empty()) {
        long long before_id = 0;
        if (parse_i64(before_id_raw, before_id) && before_id > 0) {
            filter.before_id = before_id;
        }
    }
    const std::string q_model = trim_ascii(query_param_value(params, "q_model"));
    if (!q_model.empty()) {
        filter.model_like = q_model;
    }
    filter.limit = limit + 1;

    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        auto loaded = store.query(filter);
        const bool has_extra = static_cast<int>(loaded.size()) > limit;
        if (has_extra) {
            loaded.resize(static_cast<size_t>(limit));
        }
        json body;
        json events = json::array();
        for (const Request &req : loaded) {
            events.push_back(request_to_user_event_json(req));
        }
        body["events"] = std::move(events);
        if (has_extra && !loaded.empty()) {
            body["next_before_id"] = loaded.back().id;
        } else {
            body["next_before_id"] = nullptr;
        }
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json usage_timeseries_http_response(std::string_view raw_request, std::string_view target, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return json({ { "success", false }, { "message", message } });
    }
    std::string granularity = trim_ascii(query_param_value(params, "granularity"));
    if (granularity.empty()) {
        granularity = "day";
    }
    if (granularity != "hour" && granularity != "day") {
        return json({ { "success", false }, { "message", "granularity 无效" } });
    }
    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        json body;
        body["time_zone"] = options.time_zone;
        body["start"] = options.start_utc.has_value() ? json(to_iso8601z(*options.start_utc)) : json(nullptr);
        body["end"] = options.end_exclusive_utc.has_value() ?
                          json(to_iso8601z(*options.end_exclusive_utc - std::chrono::seconds{ 1 })) :
                          json(nullptr);
        body["granularity"] = granularity;
        body["points"] = usage_time_series(rows, options.time_zone, granularity);
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json usage_event_detail_http_response(std::string_view raw_request, long long event_id, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    if (event_id <= 0) {
        return json({ { "success", false }, { "message", "event_id 无效" } });
    }
    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto req = store.get_by_id(event_id);
        if (!req.has_value() || req->user_id != user->id) {
            return json({ { "success", false }, { "message", "事件不存在" } });
        }
        json body;
        body["event_id"] = req->id;
        body["pricing_breakdown"] = to_json(compute_pricing_breakdown(*req));
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

struct AdminUsageRange {
    sys_seconds since_utc{};
    sys_seconds until_utc{};
    std::string start;
    std::string end;
    std::string since_local;
    std::string until_local;
    bool all_time = false;
};

std::optional<AdminUsageRange> resolve_admin_usage_range(const std::map<std::string, std::string> &params,
                                                         sys_seconds now_utc, std::string &error)
{
    error.clear();
    AdminUsageRange out;
    const auto today_local = date::make_zoned(std::string{ kAdminTimeZone }, now_utc).get_local_time();
    const date::year_month_day today_ymd{ date::floor<date::days>(today_local) };
    const std::string today = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d");

    const std::string all_time_raw = query_param_value(params, "all_time");
    if (!all_time_raw.empty() && !parse_bool_flag(all_time_raw, out.all_time)) {
        error = "all_time 不合法";
        return std::nullopt;
    }

    std::string start = trim_ascii(query_param_value(params, "start"));
    std::string end = trim_ascii(query_param_value(params, "end"));
    if (out.all_time) {
        RequestStore &store = UserStore::instance().tokens().requests();
        RequestListFilter filter;
        filter.limit = 1;
        filter.order_asc = true;
        const auto first_rows = store.query(filter);
        if (!first_rows.empty() && !first_rows.front().time.empty()) {
            try {
                start = format_local(parse_mysql_datetime(first_rows.front().time), std::string{ kAdminTimeZone },
                                     "%Y-%m-%d");
                end = today;
            } catch (const std::exception &) {
                start.clear();
                end.clear();
            }
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
    unsigned sm = static_cast<unsigned>(start_m);
    unsigned sd = static_cast<unsigned>(start_d);
    unsigned em = static_cast<unsigned>(end_m);
    unsigned ed = static_cast<unsigned>(end_d);
    out.since_utc = local_date_to_utc(start_y, sm, sd, std::string{ kAdminTimeZone });
    const sys_seconds end_start = local_date_to_utc(end_y, em, ed, std::string{ kAdminTimeZone });
    next_date(end_y, em, ed);
    const sys_seconds end_exclusive = local_date_to_utc(end_y, em, ed, std::string{ kAdminTimeZone });
    if (out.since_utc >= end_exclusive) {
        error = "start 不能晚于 end";
        return std::nullopt;
    }
    out.start = start;
    out.end = end;
    out.since_local = format_local(out.since_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d %H:%M");
    const sys_seconds today_start =
        local_date_to_utc(static_cast<int>(today_ymd.year()), static_cast<unsigned>(today_ymd.month()),
                          static_cast<unsigned>(today_ymd.day()), std::string{ kAdminTimeZone });
    if (end_start >= today_start) {
        out.end = today;
        out.until_utc = now_utc;
        out.until_local = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d %H:%M");
    } else {
        out.until_utc = end_exclusive;
        out.until_local =
            format_local(end_exclusive - std::chrono::seconds{ 1 }, std::string{ kAdminTimeZone }, "%Y-%m-%d %H:%M");
    }
    return out;
}

RequestListFilter build_admin_filter(const std::map<std::string, std::string> &params, const AdminUsageRange &range,
                                     int limit, std::string &error)
{
    odb::database &db = database();
    error.clear();
    RequestListFilter filters;
    filters.limit = limit;
    filters.start = to_mysql_datetime(range.since_utc);
    filters.end_exclusive = to_mysql_datetime(range.until_utc);

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
        filters.model_exact = model;
    }
    const std::string q_model = trim_ascii(query_param_value(params, "q_model"));
    if (!q_model.empty()) {
        filters.model_like = q_model;
    }

    const std::string q_user = trim_ascii(query_param_value(params, "q_user"));
    if (!q_user.empty()) {
        using uq = odb::query<User>;
        ScopedTransaction t(db);
        for (const User &u :
             db.query<User>(uq::email.like("%" + q_user + "%") || uq::username.like("%" + q_user + "%"))) {
            filters.user_ids.push_back(u.id);
        }
        t.commit();
        if (filters.user_ids.empty()) {
            filters.user_ids.push_back(-1);
        }
    }
    const std::string q_channel = trim_ascii(query_param_value(params, "q_channel"));
    if (!q_channel.empty()) {
        using cq = odb::query<Channel>;
        ScopedTransaction t(db);
        for (const Channel &c : db.query<Channel>(cq::name.like("%" + q_channel + "%"))) {
            filters.channel_ids.push_back(c.id);
        }
        t.commit();
        if (filters.channel_ids.empty()) {
            filters.channel_ids.push_back(-1);
        }
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
        filters.order_asc = true;
    }
    if (filters.before_id.has_value() && filters.after_id.has_value()) {
        error = "before_id 与 after_id 不能同时使用";
    }
    return filters;
}

json request_to_admin_event_json(const Request &req, std::string_view user_email, std::string_view channel_name)
{
    const revlm::UsageTokens tokens = revlm::usage_tokens(req);
    const long long cached_tokens =
        tokens.cache_read_tokens + tokens.cache_creation_5m_tokens + tokens.cache_creation_1h_tokens;
    json o = to_json(req);
    o["time"] = req.time.empty() ? std::string{} : to_iso8601z(parse_mysql_datetime(req.time));
    o["user_email"] = user_email;
    o["model"] = req.model_name.null() ? json(nullptr) : json(*req.model_name);
    if (tokens.output_tokens > 0 && req.latency_ms > 0) {
        o["tokens_per_second"] = request_detail::decimal_to_string(static_cast<double>(tokens.output_tokens) * 1000.0 /
                                                                   static_cast<double>(req.latency_ms));
    } else {
        o["tokens_per_second"] = "-";
    }
    o["cached_tokens"] = cached_tokens;
    o["cost_usd"] = request_detail::decimal_to_string(req.solve_price());
    o["upstream_channel_name"] = channel_name;
    o["response_id"] = req.response_id.null() ? json(nullptr) : json(*req.response_id);
    const auto error_class = nullable_odb_string(req.error_class);
    const auto error_message = nullable_odb_string(req.error_message);
    std::string error;
    if (error_class.has_value() && error_message.has_value()) {
        error = *error_class + " (" + *error_message + ")";
    } else if (error_class.has_value()) {
        error = *error_class;
    } else if (error_message.has_value()) {
        error = *error_message;
    }
    o["error"] = error;
    return o;
}

json admin_window_summary(const AdminUsageRange &range, const std::vector<Request> &rows,
                          const std::vector<Request> &recent_rows)
{
    long long requests = 0;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_tokens = 0;
    long long cache_creation_tokens = 0;
    long long first_token_sum = 0;
    long long first_token_samples = 0;
    long long decode_tokens = 0;
    long long decode_latency_ms = 0;
    double used = 0.0;
    for (const Request &req : rows) {
        const revlm::UsageTokens tokens = revlm::usage_tokens(req);
        ++requests;
        input_tokens += tokens.input_tokens;
        output_tokens += tokens.output_tokens;
        cache_read_tokens += tokens.cache_read_tokens;
        cache_creation_tokens += tokens.cache_creation_5m_tokens + tokens.cache_creation_1h_tokens;
        used += req.solve_price();
        if (req.first_token_latency_ms > 0) {
            first_token_sum += req.first_token_latency_ms;
            ++first_token_samples;
        }
        if (tokens.output_tokens > 0 && req.latency_ms > req.first_token_latency_ms) {
            decode_tokens += tokens.output_tokens;
            decode_latency_ms += req.latency_ms - req.first_token_latency_ms;
        }
    }
    long long recent_requests = 0;
    long long recent_tokens = 0;
    for (const Request &req : recent_rows) {
        const revlm::UsageTokens tokens = revlm::usage_tokens(req);
        ++recent_requests;
        recent_tokens += tokens.input_tokens + tokens.output_tokens;
    }
    const double total_tokens = static_cast<double>(input_tokens + output_tokens);
    const double cached_tokens = static_cast<double>(cache_read_tokens + cache_creation_tokens);
    json o;
    o["window"] = "统计区间";
    o["since"] = range.since_local;
    o["until"] = range.until_local;
    o["requests"] = requests;
    o["tokens"] = input_tokens + output_tokens;
    o["input_tokens"] = input_tokens;
    o["output_tokens"] = output_tokens;
    o["cached_tokens"] = cache_read_tokens + cache_creation_tokens;
    o["cache_ratio"] =
        request_detail::decimal_to_string((total_tokens > 0 ? cached_tokens / total_tokens : 0.0) * 100.0);
    o["rpm"] = request_detail::decimal_to_string(static_cast<double>(recent_requests));
    o["tpm"] = request_detail::decimal_to_string(static_cast<double>(recent_tokens));
    o["avg_first_token_latency"] = request_detail::decimal_to_string(
        first_token_samples > 0 ? static_cast<double>(first_token_sum) / static_cast<double>(first_token_samples) :
                                  0.0);
    o["tokens_per_second"] = request_detail::decimal_to_string(
        decode_latency_ms > 0 ? static_cast<double>(decode_tokens) * 1000.0 / static_cast<double>(decode_latency_ms) :
                                0.0);
    o["usd"] = request_detail::decimal_to_string(used);
    return o;
}

json top_users_json(const std::vector<Request> &rows)
{
    struct Acc {
        std::string email;
        std::string role;
        long long status = 0;
        double used = 0.0;
    };
    std::map<long long, Acc> by_user;
    UserStore &users = UserStore::instance();
    for (const Request &req : rows) {
        Acc &acc = by_user[req.user_id];
        if (acc.email.empty()) {
            const User u = users.get_user_by_id(req.user_id);
            acc.email = u.email;
            acc.role = u.role;
            acc.status = u.status;
        }
        acc.used += req.solve_price();
    }
    std::vector<std::pair<long long, Acc>> ranked(by_user.begin(), by_user.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        if (a.second.used != b.second.used) {
            return a.second.used > b.second.used;
        }
        return a.first > b.first;
    });
    if (ranked.size() > 50) {
        ranked.resize(50);
    }
    json out = json::array();
    for (const auto &entry : ranked) {
        json o;
        o["user_id"] = entry.first;
        o["email"] = entry.second.email;
        o["role"] = entry.second.role;
        o["status"] = entry.second.status;
        o["usd"] = request_detail::decimal_to_string(entry.second.used);
        out.push_back(std::move(o));
    }
    return out;
}

json admin_dashboard_http_response(std::string_view raw_request, std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    try {
        UserStore &users = UserStore::instance();
        ChannelStore &channels = ChannelStore::instance();
        const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto local = date::make_zoned(std::string{ kAdminTimeZone }, now_utc).get_local_time();
        const date::year_month_day ymd{ date::floor<date::days>(local) };
        const sys_seconds today_start =
            local_date_to_utc(static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                              static_cast<unsigned>(ymd.day()), std::string{ kAdminTimeZone });

        RequestListFilter filter;
        filter.start = to_mysql_datetime(today_start);
        filter.end_exclusive = to_mysql_datetime(now_utc);
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filter);
        long long requests_today = 0;
        long long input_tokens = 0;
        long long output_tokens = 0;
        double cost = 0.0;
        for (const Request &req : rows) {
            const revlm::UsageTokens tokens = revlm::usage_tokens(req);
            ++requests_today;
            input_tokens += tokens.input_tokens;
            output_tokens += tokens.output_tokens;
            cost += req.solve_price();
        }
        json stats;
        stats["users_count"] = users.count_users();
        const auto channel_list = channels.list_channels();
        stats["channels_count"] = static_cast<long long>(channel_list.size());
        stats["endpoints_count"] = static_cast<long long>(channel_list.size());
        stats["requests_today"] = requests_today;
        stats["tokens_today"] = input_tokens + output_tokens;
        stats["input_tokens_today"] = input_tokens;
        stats["output_tokens_today"] = output_tokens;
        stats["cost_today"] = request_detail::decimal_to_string(cost);
        json data;
        data["admin_time_zone"] = kAdminTimeZone;
        data["stats"] = std::move(stats);
        return json({ { "success", true }, { "data", std::move(data) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "读取统计失败" } });
    }
}

json admin_usage_page_http_response(std::string_view raw_request, std::string_view target, std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    const auto params = parse_query_map(target);
    int limit = 50;
    const std::string limit_raw = query_param_value(params, "limit");
    if (!limit_raw.empty() && !parse_i32(limit_raw, limit)) {
        return json({ { "success", false }, { "message", "limit 不合法" } });
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
        return json({ { "success", false }, { "message", "summary 不合法" } });
    }

    try {
        const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        std::string range_error;
        const auto range = resolve_admin_usage_range(params, now_utc, range_error);
        if (!range.has_value()) {
            return json({ { "success", false }, { "message", range_error } });
        }
        std::string filter_error;
        RequestListFilter page_filter = build_admin_filter(params, *range, limit + 1, filter_error);
        if (!filter_error.empty()) {
            return json({ { "success", false }, { "message", filter_error } });
        }
        RequestStore &store = UserStore::instance().tokens().requests();
        auto loaded = store.query(page_filter);
        const bool after = page_filter.after_id.has_value();
        if (after) {
            std::reverse(loaded.begin(), loaded.end());
        }
        const bool has_extra = static_cast<int>(loaded.size()) > limit;
        if (has_extra) {
            loaded.resize(static_cast<size_t>(limit));
        }

        std::map<long long, std::string> emails;
        std::map<long long, std::string> channel_names;
        UserStore &users = UserStore::instance();
        ChannelStore &channels = ChannelStore::instance();
        for (const Channel &c : channels.list_channels()) {
            channel_names[c.id] = c.name;
        }

        json events = json::array();
        for (const Request &req : loaded) {
            if (!emails.contains(req.user_id)) {
                emails[req.user_id] = users.get_user_by_id(req.user_id).email;
            }
            events.push_back(request_to_admin_event_json(
                req, emails[req.user_id], channel_names.contains(req.channel_id) ? channel_names[req.channel_id] : ""));
        }

        json data;
        data["admin_time_zone"] = kAdminTimeZone;
        data["now"] = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d %H:%M");
        data["start"] = range->start;
        data["end"] = range->end;
        data["limit"] = limit;
        data["events"] = std::move(events);
        if (has_extra && !loaded.empty()) {
            data["next_before_id"] = loaded.back().id;
        } else {
            data["next_before_id"] = nullptr;
        }
        if ((after || page_filter.before_id.has_value()) && !loaded.empty()) {
            data["prev_after_id"] = loaded.front().id;
        } else {
            data["prev_after_id"] = nullptr;
        }
        data["cursor_active"] = page_filter.before_id.has_value() || page_filter.after_id.has_value();

        if (include_summary) {
            RequestListFilter summary_filter = page_filter;
            summary_filter.limit = 0;
            summary_filter.before_id.reset();
            summary_filter.after_id.reset();
            summary_filter.order_asc = false;
            const auto summary_rows = store.query(summary_filter);
            RequestListFilter recent_filter;
            recent_filter.start = to_mysql_datetime(now_utc - std::chrono::seconds{ 60 });
            recent_filter.end_exclusive = to_mysql_datetime(now_utc + std::chrono::seconds{ 1 });
            const auto recent_rows = store.query(recent_filter);
            data["window"] = admin_window_summary(*range, summary_rows, recent_rows);
            data["top_users"] = top_users_json(summary_rows);
        }
        return json({ { "success", true }, { "data", std::move(data) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json admin_usage_event_detail_http_response(std::string_view raw_request, long long event_id, std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    if (event_id <= 0) {
        return json({ { "success", false }, { "message", "event_id 不合法" } });
    }
    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto req = store.get_by_id(event_id);
        if (!req.has_value()) {
            return json({ { "success", false }, { "message", "not found" } });
        }
        json body;
        body["event_id"] = req->id;
        body["pricing_breakdown"] = to_json(compute_pricing_breakdown(*req));
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "查询失败" } });
    }
}

json admin_usage_timeseries_http_response(std::string_view raw_request, std::string_view target,
                                          std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    std::map<std::string, std::string> params = parse_query_map(target);
    std::string granularity = std::string{ trim_ascii(query_param_value(params, "granularity")) };
    if (granularity.empty()) {
        granularity = "hour";
    }
    if (granularity != "hour" && granularity != "day") {
        return json({ { "success", false }, { "message", "granularity 仅支持 hour/day" } });
    }
    bool all_time = false;
    const std::string all_time_raw = query_param_value(params, "all_time");
    if (!all_time_raw.empty() && !parse_bool_flag(all_time_raw, all_time)) {
        return json({ { "success", false }, { "message", "all_time 不合法" } });
    }
    const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    if (query_param_value(params, "start").empty() && query_param_value(params, "end").empty() && !all_time) {
        if (granularity == "day") {
            params["start"] = format_local(now_utc - std::chrono::seconds{ 29 * 24 * 3600 },
                                           std::string{ kAdminTimeZone }, "%Y-%m-%d");
            params["end"] = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d");
        } else {
            params["start"] = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d");
            params["end"] = params["start"];
        }
    }
    try {
        std::string range_error;
        const auto range = resolve_admin_usage_range(params, now_utc, range_error);
        if (!range.has_value()) {
            return json({ { "success", false }, { "message", range_error } });
        }
        std::string filter_error;
        RequestListFilter filters = build_admin_filter(params, *range, 0, filter_error);
        if (!filter_error.empty()) {
            return json({ { "success", false }, { "message", filter_error } });
        }
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filters);
        json body;
        body["admin_time_zone"] = kAdminTimeZone;
        body["start"] = range->start;
        body["end"] = range->end;
        body["granularity"] = granularity;
        body["points"] = usage_time_series(rows, std::string{ kAdminTimeZone }, granularity);
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "查询失败" } });
    }
}

std::string plugin_asset_content_type(std::string_view path)
{
    const std::string lower = lowercase_ascii(path);
    if (lower.ends_with(".js") || lower.ends_with(".mjs")) {
        return "text/javascript; charset=utf-8";
    }
    if (lower.ends_with(".css")) {
        return "text/css; charset=utf-8";
    }
    if (lower.ends_with(".json")) {
        return "application/json; charset=utf-8";
    }
    if (lower.ends_with(".wasm")) {
        return "application/wasm";
    }
    if (lower.ends_with(".svg")) {
        return "image/svg+xml";
    }
    if (lower.ends_with(".png")) {
        return "image/png";
    }
    if (lower.ends_with(".jpg") || lower.ends_with(".jpeg")) {
        return "image/jpeg";
    }
    if (lower.ends_with(".webp")) {
        return "image/webp";
    }
    if (lower.ends_with(".avif")) {
        return "image/avif";
    }
    if (lower.ends_with(".gif")) {
        return "image/gif";
    }
    if (lower.ends_with(".ico")) {
        return "image/x-icon";
    }
    if (lower.ends_with(".woff2")) {
        return "font/woff2";
    }
    if (lower.ends_with(".woff")) {
        return "font/woff";
    }
    if (lower.ends_with(".ttf")) {
        return "font/ttf";
    }
    return "application/octet-stream";
}

// -- plugin admin + frontend responses --------------------------------------

namespace fs = std::filesystem;

bool plugin_marker_exists(std::string_view plugin_dir, std::string_view dir, std::string_view id)
{
    std::error_code error;
    return fs::is_regular_file(fs::path{ plugin_dir } / std::string{ dir } / std::string{ id }, error);
}

// A package is enabled unless it carries a disabled or pending-uninstall marker,
// matching the plugin runtime's enable rule. Failed-cleanup keeps the disabled
// marker, so "failed" is reported only as a state, never as enabled.
std::string plugin_state(std::string_view plugin_dir, std::string_view id)
{
    if (plugin_marker_exists(plugin_dir, "pending", id)) {
        return "pending";
    }
    if (plugin_marker_exists(plugin_dir, "failed", id)) {
        return "failed";
    }
    if (plugin_marker_exists(plugin_dir, "disabled", id)) {
        return "disabled";
    }
    return "enabled";
}

std::string plugin_failed_error(std::string_view plugin_dir, std::string_view id)
{
    std::ifstream input(fs::path{ plugin_dir } / "failed" / std::string{ id });
    if (!input) {
        return {};
    }
    std::string line;
    std::getline(input, line);
    return trim_ascii(line);
}

json plugin_action_json(const plugin::PluginActionResult &result)
{
    return json({ { "success", result.ok }, { "message", result.message } });
}

// Every installed package (enabled or not) from both roots; a user package
// overrides a system package with the same manifest id. The second element is
// whether the surviving root belongs to the read-only system directory.
std::map<std::string, std::pair<fs::path, bool>> plugin_package_roots()
{
    std::map<std::string, std::pair<fs::path, bool>> roots;
    const auto scan = [&roots](const std::string &base, bool system) {
        std::error_code error;
        const fs::path dir = fs::path{ base } / "packages";
        if (!fs::is_directory(dir, error)) {
            return;
        }
        for (const fs::directory_entry &entry : fs::directory_iterator(dir, error)) {
            if (error) {
                break;
            }
            const std::string id = entry.path().filename().string();
            if (entry.is_directory(error) && plugin::plugin_identifier_is_safe(id)) {
                roots[id] = { entry.path(), system }; // later (user) scan overrides earlier (system)
            }
        }
    };
    scan(config().system_plugin_dir, /*system=*/true);
    scan(config().plugin_dir, /*system=*/false);
    return roots;
}

json admin_plugins_response(std::string_view raw_request, std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    try {
        const auto roots = plugin_package_roots();
        json items = json::array();
        for (const auto &[id, entry] : roots) {
            const fs::path &root = entry.first;
            json item;
            item["id"] = id;
            json manifest_error = json(nullptr);
            try {
                const plugin::PluginPackage package = plugin::read_plugin_package(root);
                item["name"] = package.name;
                item["description"] = package.description;
                item["version"] = package.version;
                item["type"] = package.type;
            } catch (const std::exception &err) {
                manifest_error = trim_ascii(err.what());
            }
            const std::string plugin_dir = config().plugin_dir;
            const std::string state = plugin_state(plugin_dir, id);
            item["state"] = state;
            item["enabled"] = (state == "enabled");
            item["system_plugin"] = entry.second;
            if (state == "failed") {
                item["error"] = plugin_failed_error(plugin_dir, id);
            } else if (!manifest_error.is_null()) {
                item["error"] = manifest_error;
            } else {
                item["error"] = json(nullptr);
            }
            items.push_back(std::move(item));
        }
        return json({ { "success", true }, { "data", std::move(items) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", trim_ascii(err.what()) } });
    }
}

json admin_plugin_upload_response(std::string_view raw_request, std::string_view filename, std::string_view archive,
                                  std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    const std::string normalized = trim_ascii(filename);
    if (!normalized.ends_with(".revlm-plugin")) {
        return json({ { "success", false }, { "message", "只接受 .revlm-plugin ZIP 包" } });
    }
    return plugin_action_json(plugin::install_plugin_archive(archive));
}

json admin_plugin_enable_response(std::string_view raw_request, std::string_view plugin_id, bool enabled,
                                  std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    return plugin_action_json(plugin::set_plugin_enabled(plugin_id, enabled));
}

json admin_plugin_uninstall_response(std::string_view raw_request, std::string_view plugin_id, std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    return plugin_action_json(plugin::schedule_plugin_uninstall(plugin_id));
}

json plugin_frontend_entries_response()
{
    return json({ { "success", true }, { "data", plugin::plugin_frontend_entries_json() } });
}

} // namespace

ProxyRequest make_request(const ::httplib::Request &req, std::string_view request_id)
{
    static std::atomic<long long> request_counter{ 0 };
    ProxyRequest pr;
    pr.id = ++request_counter;
    pr.request_id = request_id.empty() ? resolve_request_id(req) : std::string{ request_id };
    pr.time = to_mysql_datetime(std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()));
    pr.http.method = req.method;
    pr.http.path = req.path;
    pr.http.body = req.body;
    pr.http.client_ip = req.remote_addr.empty() ? "127.0.0.1" : req.remote_addr;
    for (const auto &entry : req.headers) {
        const std::string lower = lowercase_ascii(entry.first);
        if (lower == "authorization" || lower == "x-api-key" || lower == "x-client-request-id") {
            continue;
        }
        pr.http.headers.emplace_back(entry.first, entry.second);
    }
    // Ensure X-Request-Id is present in headers for upstream forwarding.
    {
        bool has_request_id = false;
        for (const auto &kv : pr.http.headers) {
            if (lowercase_ascii(kv.first) == "x-request-id") {
                has_request_id = true;
                break;
            }
        }
        if (!has_request_id) {
            pr.http.headers.emplace_back("X-Request-Id", pr.request_id);
        }
    }
    return pr;
}

// Core-owned single /v1 data-plane entry (ADR-0003). The core only authenticates
// the API key, resolves the ChannelGroup snapshot into the ProxyRequest, then
// calls the interposable revlm_handle_v1 hook. Protocol routing (/v1/models,
// /v1/chat/completions, ...) belongs to the plugin hook chain, not the core.
::httplib::Server::Handler make_v1_handler()
{
    return make_http_handler([](const ::httplib::Request &req, ::httplib::Response &res, RequestContext & /* ctx */) {
        ProxyRequest proxy = make_request(req, res.get_header_value("X-Request-Id"));
        long long user_id = 0;
        long long token_id = 0;
        const auto channel_group_id = authenticate_api_token(req, user_id, token_id);
        if (!channel_group_id.has_value()) {
            write_json(res, 401, json{ { "error", json{ { "message", "Unauthorized" } } } });
            return;
        }
        proxy.auth.user_id = user_id;
        proxy.auth.token_id = token_id;
        proxy.auth.channel_group_id = *channel_group_id;

        // Resolve the ChannelGroup snapshot for the plugin hook. Only a
        // status-enabled group with at least one member routes; otherwise
        // the hook sees an empty snapshot and the plugin cannot proceed.
        const ChannelGroup group = ChannelGroupStore::instance().get_channel_group_by_id(*channel_group_id);
        if (group.id > 0 && group.status) {
            proxy.channel_group.id = group.id;
            proxy.channel_group.type = group.type;
            proxy.channel_group.price_multiplier = group.price_multiplier;
            proxy.channel_group.status = group.status;
            proxy.channel_group.channels = group.channels;
            proxy.channel_group.pointer = 0;
            proxy.upstream.channel_group_multiplier = group.price_multiplier;

            // Core pre-selects the first candidate (ADR-0003 "选中 Channel"):
            // first active member in round-robin order (ChannelGroup members
            // carry no intra-group priority). The selected Channel is written
            // into the request context before the hook runs, so the plugin's
            // first revlm_next_candidate call advances to the *next* active
            // member (round-robin tail, no re-pick).
            const auto first_active = std::find_if(proxy.channel_group.channels.begin(),
                                                   proxy.channel_group.channels.end(),
                                                   [](const Channel &candidate) { return candidate.status; });
            if (first_active != proxy.channel_group.channels.end()) {
                proxy.channel_group.pointer = static_cast<int>(first_active - proxy.channel_group.channels.begin());
                proxy.upstream.channel_id = first_active->id;
            }
        }

        // Plugin exception semantics (CONTEXT "插件异常"): catch at the HTTP
        // boundary, return 500, never auto-commit and never debit. With no
        // matching protocol plugin the core's revlm_handle_v1 fallback
        // returns 500 without committing (CONTEXT "无匹配协议插件").
        try {
            revlm_handle_v1(req, res, proxy);
        } catch (const std::exception &error) {
            std::cerr << "data-plane plugin hook failed: " << error.what() << '\n';
            write_json(res, 500, json{ { "error", json{ { "message", error.what() } } } });
        } catch (...) {
            std::cerr << "data-plane plugin hook failed: unknown\n";
            write_json(res, 500, json{ { "error", json{ { "message", "internal error" } } } });
        }
    });
}

void register_http_routes(::httplib::Server &server, const std::shared_ptr<std::atomic_bool> &draining)
{
    revlm_register_http_routes(server, draining);
}

extern "C" void revlm_register_http_routes(::httplib::Server &server, const std::shared_ptr<std::atomic_bool> &draining)
{
    auto api = [](auto fn) {
        return make_response_handler(
            [fn = std::move(fn)](const ::httplib::Request &req, RequestContext &ctx) -> json { return fn(req, ctx); });
    };

    server.Get("/readyz", make_http_handler([draining](const ::httplib::Request &, ::httplib::Response &res,
                                                       RequestContext & /* ctx */) {
                   if (draining->load()) {
                       res.status = 503;
                       res.reason = "Service Unavailable";
                       res.set_content("draining", "text/plain; charset=utf-8");
                       return;
                   }
                   res.status = 200;
                   res.reason = "OK";
                   res.set_content("ok", "text/plain; charset=utf-8");
               }));
    server.Get("/api/user/self", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return self_response(ctx.raw_request, &ctx.set_cookie);
               }));
    server.Get("/api/user/logout", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return logout_response(ctx.raw_request, &ctx.set_cookie);
               }));
    server.Get("/api/user/models/detail", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return user_models_detail_http_response(ctx.raw_request, &ctx.set_cookie);
               }));
    server.Get("/api/dashboard", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return dashboard_http_response(ctx.raw_request, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/request/windows", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return usage_windows_http_response(ctx.raw_request, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/request/events", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return requests_http_response(ctx.raw_request, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/request/timeseries", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return usage_timeseries_http_response(ctx.raw_request, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/request/events/:event_id/detail", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto event_id = path_param_i64(req, "event_id");
                   if (!event_id.has_value()) {
                       return json({ { "success", false }, { "message", "event_id 无效" } });
                   }
                   return usage_event_detail_http_response(ctx.raw_request, *event_id, &ctx.set_cookie);
               }));
    server.Get("/api/token", api([](const ::httplib::Request &, RequestContext &ctx) {
                   json error;
                   const auto user = api_authenticated_user(ctx.raw_request, error, &ctx.set_cookie);
                   if (!user.has_value()) {
                       return error;
                   }
                   return list_user_tokens_response(*user);
               }));
    server.Post("/api/token", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return create_user_token_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Get("/api/token/:token_id/reveal", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ?
                              reveal_user_token_response(ctx.raw_request, *token_id, &ctx.set_cookie) :
                              json({ { "success", false }, { "message", "token_id 不合法" } });
               }));
    server.Post("/api/token/:token_id/rotate", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto token_id = path_param_i64(req, "token_id");
                    return token_id.has_value() ?
                               rotate_user_token_response(ctx.raw_request, *token_id, &ctx.set_cookie) :
                               json({ { "success", false }, { "message", "token_id 不合法" } });
                }));
    server.Post("/api/token/:token_id/revoke", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto token_id = path_param_i64(req, "token_id");
                    return token_id.has_value() ?
                               revoke_user_token_response(ctx.raw_request, *token_id, &ctx.set_cookie) :
                               json({ { "success", false }, { "message", "token_id 不合法" } });
                }));
    server.Delete("/api/token/:token_id", api([](const ::httplib::Request &req, RequestContext &ctx) {
                      const auto token_id = path_param_i64(req, "token_id");
                      return token_id.has_value() ?
                                 delete_user_token_response(ctx.raw_request, *token_id, &ctx.set_cookie) :
                                 json({ { "success", false }, { "message", "token_id 不合法" } });
                  }));
    server.Get("/api/token/:token_id/channel", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ? token_channel_response(ctx.raw_request, *token_id, &ctx.set_cookie) :
                                                 json({ { "success", false }, { "message", "token_id 不合法" } });
               }));
    server.Put("/api/token/:token_id/channel", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ?
                              set_token_channel_response(ctx.raw_request, *token_id, req.body, &ctx.set_cookie) :
                              json({ { "success", false }, { "message", "token_id 不合法" } });
               }));
    server.Post("/api/user/register", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return register_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Post("/api/user/login", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return login_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Post("/api/account/email", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return account_email_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Post("/api/account/password", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return account_password_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    // /v1/* is the core-owned special data-plane entry (ADR-0003). A single
    // prefix route authenticates the API key, resolves the ChannelGroup, and
    // enters the interposable revlm_handle_v1 hook. The plugin hook owns all
    // protocol endpoint branching (including /v1/models). Plugins must not
    // register the same /v1 path on the shared global Server.
    const auto v1_handler = make_v1_handler();
    server.Get(R"(/v1/.*)", v1_handler);
    server.Post(R"(/v1/.*)", v1_handler);
    server.Put(R"(/v1/.*)", v1_handler);
    server.Delete(R"(/v1/.*)", v1_handler);
    server.Patch(R"(/v1/.*)", v1_handler);
    server.Options(R"(/v1/.*)", v1_handler);
    server.Get("/api/admin/dashboard", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return admin_dashboard_http_response(ctx.raw_request, &ctx.set_cookie);
               }));
    server.Get("/api/admin/plugins", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return admin_plugins_response(ctx.raw_request, &ctx.set_cookie);
               }));
    server.Post("/api/admin/plugins/upload", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return admin_plugin_upload_response(ctx.raw_request, req.get_header_value("X-Plugin-Filename"),
                                                        req.body, &ctx.set_cookie);
                }));
    server.Post("/api/admin/plugins/:plugin_id/enable", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto it = req.path_params.find("plugin_id");
                    return it == req.path_params.end() ?
                               json({ { "success", false }, { "message", "插件 ID 无效" } }) :
                               admin_plugin_enable_response(ctx.raw_request, it->second, true, &ctx.set_cookie);
                }));
    server.Post("/api/admin/plugins/:plugin_id/disable", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto it = req.path_params.find("plugin_id");
                    return it == req.path_params.end() ?
                               json({ { "success", false }, { "message", "插件 ID 无效" } }) :
                               admin_plugin_enable_response(ctx.raw_request, it->second, false, &ctx.set_cookie);
                }));
    server.Delete("/api/admin/plugins/:plugin_id", api([](const ::httplib::Request &req, RequestContext &ctx) {
                      const auto it = req.path_params.find("plugin_id");
                      return it == req.path_params.end() ?
                                 json({ { "success", false }, { "message", "插件 ID 无效" } }) :
                                 admin_plugin_uninstall_response(ctx.raw_request, it->second, &ctx.set_cookie);
                  }));

    // A package frontend is arbitrary JavaScript, not a declarative channel
    // schema. The core discovers its conventional entry file and serves all
    // sibling assets from the exact worker preload snapshot.
    server.Get("/api/plugins/frontend",
               api([](const ::httplib::Request &, RequestContext &) { return plugin_frontend_entries_response(); }));
    server.Get(R"(/api/plugins/frontend/([A-Za-z0-9._-]+)/(.+))",
               make_http_handler([](const ::httplib::Request &req, ::httplib::Response &res, RequestContext &) {
                   if (req.matches.size() != 3) {
                       res.status = 404;
                       return;
                   }
                   const auto asset = plugin::plugin_frontend_file(req.matches[1].str(), req.matches[2].str());
                   if (!asset.has_value()) {
                       res.status = 404;
                       return;
                   }
                   std::ifstream input(*asset, std::ios::binary);
                   if (!input) {
                       res.status = 404;
                       return;
                   }
                   const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
                   res.status = 200;
                   res.set_content(source, plugin_asset_content_type(asset->string()));
               }));
    server.Get("/api/admin/request", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return admin_usage_page_http_response(ctx.raw_request, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/admin/request/timeseries", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return admin_usage_timeseries_http_response(ctx.raw_request, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/admin/request/events/:event_id/detail",
               api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto event_id = path_param_i64(req, "event_id");
                   return event_id.has_value() ?
                              admin_usage_event_detail_http_response(ctx.raw_request, *event_id, &ctx.set_cookie) :
                              json({ { "success", false }, { "message", "event_id 无效" } });
               }));
    server.Get("/api/admin/users", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return admin_list_users_response(ctx.raw_request, &ctx.set_cookie);
               }));
    server.Post("/api/admin/users", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return admin_create_user_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Put("/api/admin/users/:user_id", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto user_id = path_param_i64(req, "user_id");
                   return user_id.has_value() ?
                              admin_update_user_response(*user_id, ctx.raw_request, req.body, &ctx.set_cookie) :
                              json({ { "success", false }, { "message", "用户不存在" } });
               }));
    server.Delete("/api/admin/users/:user_id", api([](const ::httplib::Request &req, RequestContext &ctx) {
                      const auto user_id = path_param_i64(req, "user_id");
                      return user_id.has_value() ?
                                 admin_delete_user_response(*user_id, ctx.raw_request, &ctx.set_cookie) :
                                 json({ { "success", false }, { "message", "用户不存在" } });
                  }));
    server.Post("/api/admin/users/:user_id/password", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto user_id = path_param_i64(req, "user_id");
                    return user_id.has_value() ? admin_reset_user_password_response(*user_id, ctx.raw_request, req.body,
                                                                                    &ctx.set_cookie) :
                                                 json({ { "success", false }, { "message", "用户不存在" } });
                }));
    server.Post("/api/admin/users/:user_id/balance", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto user_id = path_param_i64(req, "user_id");
                    return user_id.has_value() ?
                               admin_add_user_balance_response(*user_id, ctx.raw_request, req.body, &ctx.set_cookie) :
                               json({ { "success", false }, { "message", "用户不存在" } });
                }));

    server.Get("/api/billing/balance", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return billing_balance_response(ctx.raw_request, &ctx.set_cookie);
               }));

    auto channel_groups = api([](const ::httplib::Request &req, RequestContext &ctx) {
        const ChannelGroupsParsedRequest parsed{ ctx.parsed.method, ctx.parsed.path, ctx.parsed.target };
        return channel_groups_route(ctx.raw_request, req.body, parsed, &ctx.set_cookie);
    });
    server.Get(R"(/api/admin/channel-groups.*)", channel_groups);
    server.Post(R"(/api/admin/channel-groups.*)", channel_groups);
    server.Put(R"(/api/admin/channel-groups.*)", channel_groups);
    server.Delete(R"(/api/admin/channel-groups.*)", channel_groups);

    auto channels = api([](const ::httplib::Request &req, RequestContext &ctx) {
        const ChannelParsedRequest parsed{ ctx.parsed.method, ctx.parsed.path, ctx.parsed.target };
        return channel_route(ctx.raw_request, req.body, parsed, &ctx.set_cookie);
    });
    server.Get(R"(/api/channel.*)", channels);
    server.Post(R"(/api/channel.*)", channels);
    server.Put(R"(/api/channel.*)", channels);
    server.Delete(R"(/api/channel.*)", channels);
}

std::string handle_http_request(std::string_view request, bool draining)
{
    InMemoryHttpServer server;
    auto draining_flag = std::make_shared<std::atomic_bool>(draining);
    server.set_keep_alive_max_count(1);
    server.set_payload_max_length(std::max(static_cast<size_t>(config().http_max_body_bytes),
                                           static_cast<size_t>(config().plugin_max_archive_bytes)));
    register_http_routes(server, draining_flag);

    ::httplib::detail::BufferStream stream;
    (void)stream.write(request.data(), request.size());
    const bool ok = server.process(stream, [](::httplib::Request &req) {
        req.remote_addr = "127.0.0.1";
        req.remote_port = 0;
    });

    const std::string &buffer = stream.get_buffer();
    if (!ok || buffer.size() <= request.size()) {
        return serialize_json_http_bytes(400, "Bad Request", json("bad request"));
    }
    return buffer.substr(request.size());
}

std::string inject_request_metadata(std::string_view request, std::string_view client_ip)
{
    std::string enriched{ request };
    const size_t request_line_end = enriched.find("\r\n");
    if (request_line_end == std::string::npos || client_ip.empty()) {
        return enriched;
    }
    enriched.insert(request_line_end + 2, "X-Revlm-Remote-Ip: " + std::string{ client_ip } + "\r\n");
    enriched.insert(request_line_end + 2, "X-Revlm-Client-Ip: " + std::string{ client_ip } + "\r\n");
    return enriched;
}

} // namespace revlm
