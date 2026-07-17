#include "proxy/anthropics_messages.hpp"

#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "models/models.hpp"
#include "proxy/upstream.hpp"
#include "proxy/gateway.hpp"
#include "request/request.hpp"
#include "server/http_server.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <algorithm>
#include <boost/json/object.hpp>
#include <boost/json/string_view.hpp>
#include <functional>
#include <httplib.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revlm
{
namespace
{

long long json_int64_or(const boost::json::object &obj, boost::json::string_view key, long long fallback = 0)
{
    const auto *value = obj.if_contains(key);
    if (value == nullptr) {
        return fallback;
    }
    if (const auto *i = value->if_int64()) {
        return *i;
    }
    if (const auto *u = value->if_uint64()) {
        return static_cast<long long>(*u);
    }
    return fallback;
}

std::optional<boost::json::object> extract_usage_object(const boost::json::value &value)
{
    if (value.is_object()) {
        const auto &obj = value.as_object();
        if (const auto *usage = obj.if_contains("usage"); usage != nullptr && usage->is_object()) {
            return usage->as_object();
        }
        for (const auto &field : obj) {
            if (auto nested = extract_usage_object(field.value())) {
                return nested;
            }
        }
        return std::nullopt;
    }
    if (value.is_array()) {
        for (const auto &child : value.as_array()) {
            if (auto nested = extract_usage_object(child)) {
                return nested;
            }
        }
    }
    return std::nullopt;
}

int merge_token(int current, long long incoming)
{
    if (incoming > 0) {
        return static_cast<int>(incoming);
    }
    return current;
}

bool channel_ok_for_anthropic(const Channel &channel)
{
    return channel.status && channel.type == 4 && !trim_ascii(channel.api_key).empty();
}

HttpResponse proxy_upstream_failed_response(std::string_view request_id)
{
    return http_response(
        502, "Bad Gateway",
        boost::json::object{ { "error", boost::json::object{ { "message", "proxy upstream failed" } } } },
        { { "X-Request-Id", std::string{ request_id } } });
}

std::optional<std::string> select_messages_proxy_model(std::string_view body)
{
    const auto model = parse_json_string_field(body, "model");
    if (!model.has_value()) {
        return std::nullopt;
    }

    const std::vector<Model> &catalog = ModelManager::instance().models();
    if (std::ranges::find(catalog, *model, &Model::name) == catalog.end()) {
        return std::nullopt;
    }
    return *model;
}

double channel_price_multiplier(long long channel_id)
{
    const auto channel = ChannelStore::instance().find_channel(channel_id);
    return channel.has_value() ? channel->price_multiplier : 1.0;
}

std::optional<ChannelGroup> load_messages_channel_group(long long channel_group_id)
{
    if (channel_group_id <= 0) {
        return std::nullopt;
    }
    ChannelGroup group = ChannelGroupStore::instance().get_channel_group_by_id(channel_group_id);
    if (group.id <= 0 || group.status == 0 || group.channels.empty()) {
        return std::nullopt;
    }
    if (group.pointer < 0 || group.pointer >= static_cast<int>(group.channels.size())) {
        group.pointer = 0;
    }
    return group;
}

} // namespace

void AnthropicsMessages::finalize(boost::json::object &json)
{
    auto usage_opt = extract_usage_object(json);
    if (!usage_opt.has_value()) {
        return;
    }
    const boost::json::object &usage = *usage_opt;
    long long ephemeral_1h = 0;
    long long ephemeral_5m = 0;
    if (const auto *cache_creation = usage.if_contains("cache_creation");
        cache_creation != nullptr && cache_creation->is_object()) {
        const boost::json::object &cache = cache_creation->as_object();
        ephemeral_1h = json_int64_or(cache, "ephemeral_1h_input_tokens");
        ephemeral_5m = json_int64_or(cache, "ephemeral_5m_input_tokens");
    }
    const int input_tokens = merge_token(request.input_tokens, json_int64_or(usage, "input_tokens"));
    const int output_tokens = merge_token(request.output_tokens, json_int64_or(usage, "output_tokens"));
    const int cache_read_tokens =
        merge_token(request.cache_read_tokens, json_int64_or(usage, "cache_read_input_tokens"));
    const int cache_creation_1h_tokens = merge_token(request.cache_creation_1h_tokens, ephemeral_1h);
    const int cache_creation_5m_tokens = merge_token(request.cache_creation_5m_tokens, ephemeral_5m);
    request.input_tokens = input_tokens;
    request.output_tokens = output_tokens;
    request.cache_read_tokens = cache_read_tokens;
    request.cache_creation_1h_tokens = cache_creation_1h_tokens;
    request.cache_creation_5m_tokens = cache_creation_5m_tokens;
}

HttpResponse run_messages_gateway(const ::httplib::Request &req, std::string_view request_id,
                                  long long channel_group_id, Request &usage)
{
    const auto model = select_messages_proxy_model(req.body);
    if (!model.has_value()) {
        return http_response(
            400, "Bad Request",
            boost::json::object{
                { "error", boost::json::object{ { "message", "messages model unavailable on anthropic channels" } } } },
            { { "X-Request-Id", std::string{ request_id } } });
    }

    if (revlm::parse_json_bool_field(req.body, "stream").value_or(false)) {
        return http_response(
            400, "Bad Request",
            boost::json::object{
                { "error", boost::json::object{ { "message", "streaming requires live socket path" } } } },
            { { "X-Request-Id", std::string{ request_id } } });
    }

    auto group = load_messages_channel_group(channel_group_id);
    if (!group.has_value()) {
        return http_response(
            400, "Bad Request",
            boost::json::object{ { "error", boost::json::object{ { "message", "channel group unavailable" } } } },
            { { "X-Request-Id", std::string{ request_id } } });
    }

    const int start = group->pointer;
    HttpResponse last_failure = proxy_upstream_failed_response(request_id);
    bool tried = false;

    do {
        Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
        if (channel_ok_for_anthropic(channel)) {
            tried = true;
            UpstreamRequest downstream = build_proxy_upstream_request(
                req, "/v1/messages", request_id, req.get_header_value("X-Revlm-Client-Ip"), req.body,
                [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });

            const ScheduledUpstreamExecution executed = execute_scheduled_upstream(channel.id, std::move(downstream));
            if (executed.result.has_value() && executed.result->response.status_code < 400) {
                const int status = executed.result->response.status_code;
                std::string body_bytes = std::move(executed.result->response.body);
                const std::vector<UpstreamHeader> &response_headers = executed.result->response.headers;
                const std::string response_id = upstream_response_id_from_headers(response_headers);

                usage.pricing_model = billing_model_for_name(*model);
                usage.model_name = *model;
                usage.channel_id = channel.id;
                usage.status_code = status;
                usage.channel_multiplier = channel_price_multiplier(channel.id);
                usage.is_stream = false;
                assign_request_correlation(usage, request_id, response_id);
                if (const auto response_tier = parse_json_string_field(body_bytes, "service_tier");
                    response_tier.has_value()) {
                    usage.service_tier = normalize_usage_service_tier(std::string_view{ *response_tier });
                }
                if (usage.pricing_model != nullptr) {
                    parse_billing_request_from_body(usage, GatewayStreamKind::anthropics_messages, body_bytes);
                }

                return make_upstream_http_response(status, std::move(body_bytes),
                                                   merge_correlation_headers(response_headers, request_id,
                                                                             response_id));
            }
            if (executed.result.has_value()) {
                const int status = executed.result->response.status_code;
                std::string body_bytes = std::move(executed.result->response.body);
                const std::vector<UpstreamHeader> &response_headers = executed.result->response.headers;
                const std::string response_id = upstream_response_id_from_headers(response_headers);
                last_failure =
                    make_upstream_http_response(status, std::move(body_bytes),
                                                merge_correlation_headers(response_headers, request_id, response_id));
                usage.channel_id = channel.id;
                usage.status_code = status;
            } else {
                last_failure = proxy_upstream_failed_response(request_id);
                usage.channel_id = channel.id;
                usage.status_code = 502;
            }
        }
        group->next_channel();
    } while (group->pointer != start);

    if (!tried) {
        return http_response(
            400, "Bad Request",
            boost::json::object{
                { "error", boost::json::object{ { "message", "messages requires an anthropic channel" } } } },
            { { "X-Request-Id", std::string{ request_id } } });
    }
    return last_failure;
}

void run_messages_stream(::httplib::Response &res, const ::httplib::Request &req, const GatewayParsedRequest &parsed,
                         std::string_view request_id, long long channel_group_id, std::string_view client_ip,
                         Request usage, const std::function<void(Request &)> &on_usage)
{
    (void)parsed;
    (void)client_ip;
    const auto model = select_messages_proxy_model(req.body);
    if (!model.has_value()) {
        apply_http_response(
            http_response(
                400, "Bad Request",
                boost::json::object{
                    { "error",
                      boost::json::object{ { "message", "messages model unavailable on anthropic channels" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
            res);
        return;
    }

    auto group = load_messages_channel_group(channel_group_id);
    if (!group.has_value()) {
        apply_http_response(
            http_response(
                400, "Bad Request",
                boost::json::object{ { "error", boost::json::object{ { "message", "channel group unavailable" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
            res);
        return;
    }

    const int start = group->pointer;
    HttpResponse last_failure = proxy_upstream_failed_response(request_id);
    bool tried = false;

    do {
        Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
        if (channel_ok_for_anthropic(channel)) {
            tried = true;
            UpstreamRequest downstream = build_proxy_upstream_request(
                req, "/v1/messages", request_id, req.get_header_value("X-Revlm-Client-Ip"), req.body,
                [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });
            ScheduledUpstreamStreamExecution executed =
                open_scheduled_upstream_stream(channel.id, std::move(downstream));
            if (executed.result.has_value() && executed.result->status_code < 400) {
                UpstreamStreamResponse upstream = std::move(*executed.result);
                const int status = upstream.status_code;
                const std::string response_id = upstream_response_id_from_headers(upstream.headers);
                const double route_mult = channel_price_multiplier(channel.id);
                usage.pricing_model = billing_model_for_name(*model);
                usage.model_name = *model;
                usage.channel_id = channel.id;
                usage.status_code = status;
                usage.channel_multiplier = route_mult;
                usage.is_stream = true;
                assign_request_correlation(usage, request_id, response_id);

                const std::string requested_service_tier =
                    parse_json_string_field(req.body, "service_tier").value_or("");
                const Model *pricing = usage.pricing_model;
                apply_upstream_gateway_stream(
                    res, status, upstream.headers, std::move(upstream), std::move(usage),
                    [pricing, route_mult](Request &u) -> std::unique_ptr<Gateway> {
                        if (pricing == nullptr) {
                            return nullptr;
                        }
                        return make_gateway(GatewayStreamKind::anthropics_messages, pricing, 1.0, route_mult, u);
                    },
                    requested_service_tier,
                    [status, on_usage](Request &u, const GatewayStreamResult &result) {
                        const GatewayStreamPump &pump = result.pump;
                        if (status >= 400 || !pump.completed || pump.upstream_error || pump.idle_timeout ||
                            !pump.saw_usage || u.pricing_model == nullptr) {
                            return;
                        }
                        if (!on_usage) {
                            return;
                        }
                        u.first_token_latency_ms = pump.first_token_latency_ms;
                        on_usage(u);
                    });
                set_stream_correlation_headers(res, request_id, response_id);
                return;
            }
            if (!executed.result.has_value()) {
                last_failure = proxy_upstream_failed_response(request_id);
                usage.channel_id = channel.id;
                usage.status_code = 502;
            } else {
                const int status = executed.result->status_code;
                const std::string error_body = drain_upstream_stream_body(*executed.result);
                last_failure = make_upstream_http_response(
                    status, error_body, merge_correlation_headers(executed.result->headers, request_id, {}));
                usage.channel_id = channel.id;
                usage.status_code = status;
            }
        }
        group->next_channel();
    } while (group->pointer != start);

    if (!tried) {
        apply_http_response(
            http_response(
                400, "Bad Request",
                boost::json::object{
                    { "error", boost::json::object{ { "message", "messages requires an anthropic channel" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
            res);
        return;
    }
    apply_http_response(last_failure, res);
}

} // namespace revlm
