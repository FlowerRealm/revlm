#include "channels/channels.hpp"
#include "auth/session.hpp"
#include "server/http_server.hpp"
#include "users/users.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "request/request.hpp"
#include "util/datetime.hpp"
#include "util/http_query.hpp"
#include "util/json_convert.hpp"
#include "util/user_input.hpp"
#include "util/json.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <chrono>
#include <exception>
#include <iomanip>
#include <ios>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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
};

struct RootAuth {
    bool ok = false;
    bool clear_cookie = false;
    std::string failure;
    long long actor_user_id = 0;
};

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
    long long cached_tokens = 0;
    long long first_token_samples = 0;
    long long first_token_latency_sum = 0;
    long long output_tokens = 0;
    long long decode_latency_sum = 0;
    double usd = 0.0;
    double cache_ratio = 0.0;
    double avg_first_token_latency_ms = 0.0;
    double tokens_per_second = 0.0;
};

struct ChannelRuntimeSnapshot {
    bool available = true;
    std::optional<int> fail_score;
};

HttpResponse admin_auth_failure(std::string_view request_id, std::string_view message, bool clear_cookie,
                                std::string_view raw_request)
{
    std::vector<Header> headers{ { "X-Request-Id", std::string{ request_id } } };
    if (clear_cookie) {
        headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
    }
    return http_response(200, "OK", json({ { "success", false }, { "message", message } }), std::move(headers));
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

json channel_usage_json(const ChannelUsageMetrics &usage)
{
    return json(
        { { "usd", trim_decimal_zeros(decimal_string(usage.usd, 6)) },
          { "tokens", usage.tokens },
          { "cache_ratio", trim_decimal_zeros(decimal_string(usage.cache_ratio * 100.0, 1)) },
          { "avg_first_token_latency", trim_decimal_zeros(decimal_string(usage.avg_first_token_latency_ms, 1)) },
          { "tokens_per_second", trim_decimal_zeros(decimal_string(usage.tokens_per_second, 2)) } });
}

json channel_runtime_json(const ChannelRuntimeSnapshot &runtime)
{
    return json({ { "available", runtime.available },
                  { "fail_score", runtime.fail_score.has_value() ? json(*runtime.fail_score) : json(nullptr) } });
}

json channel_json(const Channel &channel, const std::optional<bool> &in_use = std::nullopt,
                  const std::optional<ChannelUsageMetrics> &usage = std::nullopt,
                  const std::optional<ChannelRuntimeSnapshot> &runtime = std::nullopt)
{
    json body = to_json(channel);
    if (in_use.has_value()) {
        body["in_use"] = *in_use;
        body["usage"] = channel_usage_json(usage.value_or(ChannelUsageMetrics{}));
        body["runtime"] = channel_runtime_json(runtime.value_or(ChannelRuntimeSnapshot{}));
    }
    return body;
}
} // namespace

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
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        window.end = to_mysql_datetime(now);
        window.start = to_mysql_datetime(now - std::chrono::seconds{ 7 * 24 * 3600 });
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

double compute_tokens_per_second(long long output_tokens, long long decode_latency_ms)
{
    if (output_tokens <= 0 || decode_latency_ms <= 0) {
        return 0.0;
    }
    return static_cast<double>(output_tokens) * 1000.0 / static_cast<double>(decode_latency_ms);
}

ChannelRuntimeSnapshot runtime_snapshot_for_channel(const Channel &channel)
{
    ChannelRuntimeSnapshot runtime;
    runtime.available = channel.status && !trim_ascii(channel.api_key).empty();
    return runtime;
}

json channels_page_json(const ChannelPageWindow &window)
{
    ChannelStore &store = ChannelStore::instance();
    const auto channels = store.list_channels();
    ChannelGroupStore &group_store = ChannelGroupStore::instance();
    std::unordered_map<long long, bool> used_channels;
    for (const ChannelGroup &group : group_store.list_channel_groups()) {
        for (const Channel &member : group.channels) {
            used_channels[member.id] = true;
        }
    }

    RequestListFilter filter;
    if (!window.all_time) {
        filter.start = window.start;
        filter.end_exclusive = to_mysql_datetime(parse_mysql_datetime(window.end) + std::chrono::seconds{ 1 });
    }
    filter.order_asc = true;

    RequestStore &requests = UserStore::instance().tokens().requests();
    const auto rows = requests.query(filter);

    const auto add_usage = [](ChannelUsageMetrics &metrics, const Request &req) {
        ++metrics.requests;
        metrics.tokens += req.input_tokens + req.output_tokens;
        metrics.cached_tokens += req.cache_read_tokens + req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
        metrics.usd += req.solve_price();
        if (req.first_token_latency_ms > 0) {
            ++metrics.first_token_samples;
            metrics.first_token_latency_sum += req.first_token_latency_ms;
        }
        metrics.output_tokens += req.output_tokens;
        if (req.latency_ms > req.first_token_latency_ms) {
            metrics.decode_latency_sum += req.latency_ms - req.first_token_latency_ms;
        }
    };
    const auto finish_usage = [](ChannelUsageMetrics &metrics) {
        if (metrics.tokens > 0 && metrics.cached_tokens > 0) {
            metrics.cache_ratio = static_cast<double>(metrics.cached_tokens) / static_cast<double>(metrics.tokens);
        }
        if (metrics.first_token_samples > 0 && metrics.first_token_latency_sum > 0) {
            metrics.avg_first_token_latency_ms =
                static_cast<double>(metrics.first_token_latency_sum) / static_cast<double>(metrics.first_token_samples);
        }
        metrics.tokens_per_second = compute_tokens_per_second(metrics.output_tokens, metrics.decode_latency_sum);
    };

    ChannelUsageMetrics overview;
    std::unordered_map<long long, ChannelUsageMetrics> by_channel;
    for (const Request &req : rows) {
        add_usage(overview, req);
        add_usage(by_channel[req.channel_id], req);
    }
    finish_usage(overview);
    for (auto &[_, usage] : by_channel) {
        finish_usage(usage);
    }

    json overview_json = channel_usage_json(overview);
    overview_json["requests"] = overview.requests;

    json channel_list = json::array();
    for (const Channel &channel : channels) {
        channel_list.push_back(channel_json(channel, used_channels.contains(channel.id), by_channel[channel.id],
                                            runtime_snapshot_for_channel(channel)));
    }

    return json({ { "admin_time_zone", "Asia/Shanghai" },
                  { "start", window.all_time ? json(nullptr) : json(window.start) },
                  { "end", window.all_time ? json(nullptr) : json(window.end) },
                  { "overview", std::move(overview_json) },
                  { "channels", std::move(channel_list) } });
}

json channel_time_series_json(const ChannelTimeSeriesRequest &req)
{
    ChannelStore &store = ChannelStore::instance();
    const auto channel = store.find_channel(req.channel_id);
    if (!channel.has_value()) {
        throw std::invalid_argument("渠道不存在");
    }

    RequestListFilter filter;
    filter.channel_id = req.channel_id;
    if (!req.all_time) {
        filter.start = req.start;
        filter.end_exclusive = to_mysql_datetime(parse_mysql_datetime(req.end) + std::chrono::seconds{ 1 });
    }
    filter.order_asc = true;

    RequestStore &requests = UserStore::instance().tokens().requests();
    const auto rows = requests.query(filter);

    std::string start_value = req.start;
    std::string end_value = req.end;
    if (req.all_time) {
        start_value.clear();
        end_value.clear();
        for (const Request &row : rows) {
            if (row.time.empty()) {
                continue;
            }
            if (start_value.empty() || row.time < start_value) {
                start_value = row.time;
            }
            if (end_value.empty() || row.time > end_value) {
                end_value = row.time;
            }
        }
    }

    const auto add_usage = [](ChannelUsageMetrics &metrics, const Request &row) {
        ++metrics.requests;
        metrics.tokens += row.input_tokens + row.output_tokens;
        metrics.cached_tokens += row.cache_read_tokens + row.cache_creation_5m_tokens + row.cache_creation_1h_tokens;
        metrics.usd += row.solve_price();
        if (row.first_token_latency_ms > 0) {
            ++metrics.first_token_samples;
            metrics.first_token_latency_sum += row.first_token_latency_ms;
        }
        metrics.output_tokens += row.output_tokens;
        if (row.latency_ms > row.first_token_latency_ms) {
            metrics.decode_latency_sum += row.latency_ms - row.first_token_latency_ms;
        }
    };
    const auto finish_usage = [](ChannelUsageMetrics &metrics) {
        if (metrics.tokens > 0 && metrics.cached_tokens > 0) {
            metrics.cache_ratio = static_cast<double>(metrics.cached_tokens) / static_cast<double>(metrics.tokens);
        }
        if (metrics.first_token_samples > 0 && metrics.first_token_latency_sum > 0) {
            metrics.avg_first_token_latency_ms =
                static_cast<double>(metrics.first_token_latency_sum) / static_cast<double>(metrics.first_token_samples);
        }
        metrics.tokens_per_second = compute_tokens_per_second(metrics.output_tokens, metrics.decode_latency_sum);
    };

    std::map<std::string, ChannelUsageMetrics> buckets;
    for (const Request &row : rows) {
        if (row.time.size() < 13) {
            continue;
        }
        const std::string bucket = req.granularity == "day" ? row.time.substr(0, 10) + " 00:00:00" :
                                                              row.time.substr(0, 13) + ":00:00";
        add_usage(buckets[bucket], row);
    }

    json points = json::array();
    for (auto &[bucket, metrics] : buckets) {
        finish_usage(metrics);
        points.push_back(json({ { "bucket", bucket },
                                { "usd", metrics.usd },
                                { "tokens", metrics.tokens },
                                { "cache_ratio", metrics.cache_ratio * 100.0 },
                                { "avg_first_token_latency", metrics.avg_first_token_latency_ms },
                                { "tokens_per_second", metrics.tokens_per_second } }));
    }

    return json({ { "admin_time_zone", "Asia/Shanghai" },
                  { "channel_id", req.channel_id },
                  { "start", start_value },
                  { "end", end_value },
                  { "granularity", req.granularity },
                  { "points", std::move(points) } });
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
        return http_response(200, "OK", json({ { "success", false }, { "message", error } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    try {
        return http_response(200, "OK", json({ { "success", true }, { "data", channels_page_json(window) } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
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
        return http_response(200, "OK", json({ { "success", false }, { "message", error } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    try {
        return http_response(200, "OK", json({ { "success", true }, { "data", channel_time_series_json(req) } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse create_channel_response(std::string_view raw_request, std::string_view body, std::string_view request_id)
{
    RootAuth auth = authenticate_root_admin(raw_request);
    if (!auth.ok) {
        return admin_auth_failure(request_id, auth.failure, auth.clear_cookie, raw_request);
    }
    auto object = parse_json_object(body);
    if (!object.has_value()) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    try {
        const std::string type = trim_ascii(json_object_string(*object, "type"));
        const std::string name = trim_ascii(json_object_string(*object, "name"));
        const bool status = parse_bool_value(json_value_to_string((*object)["status"])).value_or(true);
        const int priority = parse_int_value(json_value_to_string((*object)["priority"])).value_or(0);
        const std::string base_url = trim_ascii(json_object_string(*object, "base_url"));
        const std::string api_key = trim_ascii(json_object_string(*object, "key"));
        const double price_multiplier = (*object)["price_multiplier"].as_double().value_or(1.0);
        Channel channel(0, type, name, status, priority, base_url, api_key, price_multiplier);

        ChannelStore &store = ChannelStore::instance();
        if (!store.create_channel(channel)) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "创建渠道失败" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        return http_response(200, "OK", json({ { "success", true }, { "data", json{ { "id", channel.id } } } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse update_channel_response(std::string_view raw_request, std::string_view body, std::string_view request_id)
{
    RootAuth auth = authenticate_root_admin(raw_request);
    if (!auth.ok) {
        return admin_auth_failure(request_id, auth.failure, auth.clear_cookie, raw_request);
    }
    auto object = parse_json_object(body);
    if (!object.has_value()) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    const auto channel_id =
        parse_long_long(!(*object)["id"].is_null() ? json_value_to_string((*object)["id"]) : std::string{});
    if (!channel_id.has_value() || *channel_id <= 0) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "渠道 ID 无效" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    try {
        ChannelStore &store = ChannelStore::instance();
        auto channel = store.find_channel(*channel_id);
        if (!channel.has_value()) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "渠道不存在" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        channel->name = trim_ascii(json_object_string(*object, "name"));
        channel->status = parse_bool_value(json_value_to_string((*object)["status"])).value_or(channel->status);
        channel->priority = parse_int_value(json_value_to_string((*object)["priority"])).value_or(channel->priority);
        channel->base_url = trim_ascii(json_object_string(*object, "base_url"));
        channel->api_key = trim_ascii(json_object_string(*object, "key"));
        channel->price_multiplier = (*object)["price_multiplier"].as_double().value_or(channel->price_multiplier);
        if (!store.update_channel(*channel)) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "渠道不存在" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        return http_response(200, "OK", json({ { "success", true } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
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
            return http_response(200, "OK", json({ { "success", false }, { "message", "渠道不存在" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        return http_response(200, "OK", json({ { "success", true } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
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
    return http_response(404, "Not Found", json("not found"), { { "X-Request-Id", std::string{ request_id } } });
}

} // namespace revlm
