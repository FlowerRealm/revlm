#include "proxy_request/anthropics_messages.hpp"

#include "channels/channels.hpp"
#include "models/models.hpp"
#include "proxy_request/api_orchestrate.hpp"
#include "proxy_request/upstream.hpp"
#include "proxy_response/api_stream.hpp"
#include "proxy_response/gateway.hpp"
#include "proxy_response/gateway_stream.hpp"
#include "proxy_response/upstream_http.hpp"
#include "request/request.hpp"
#include "server/http_server.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <boost/json/object.hpp>
#include <functional>
#include <httplib.h>

#include <algorithm>
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

std::optional<std::string> select_messages_proxy_model(std::string_view body, long long channel_id)
{
    const auto model = parse_json_string_field(body, "model");
    if (!model.has_value()) {
        return std::nullopt;
    }

    const std::vector<Model> &catalog = ModelManager::instance().models();
    if (std::ranges::find(catalog, *model, &Model::name) == catalog.end()) {
        return std::nullopt;
    }

    const auto channel = ChannelStore::instance().find_channel(channel_id);
    if (!channel.has_value() || !channel->status || trim_ascii(channel->api_key).empty()) {
        return std::nullopt;
    }

    return *model;
}

double channel_price_multiplier(long long channel_id)
{
    const auto channel = ChannelStore::instance().find_channel(channel_id);
    return channel.has_value() ? channel->price_multiplier : 1.0;
}

} // namespace

HttpResponse run_messages_gateway(const ::httplib::Request &req, std::string_view request_id, long long channel_id,
                                  Request &usage)
{
    const auto model = select_messages_proxy_model(req.body, channel_id);
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

    UpstreamRequest downstream = build_proxy_upstream_request(
        req, "/v1/messages", request_id, req.get_header_value("X-Revlm-Client-Ip"), req.body,
        [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });

    const ScheduledUpstreamExecution executed = execute_scheduled_upstream(channel_id, std::move(downstream));
    if (!executed.result.has_value()) {
        return http_response(
            502, "Bad Gateway",
            boost::json::object{ { "error", boost::json::object{ { "message", "proxy upstream failed" } } } },
            { { "X-Request-Id", std::string{ request_id } } });
    }
    const int status = executed.result->response.status_code;
    std::string body_bytes = std::move(executed.result->response.body);
    const std::vector<UpstreamHeader> &response_headers = executed.result->response.headers;
    const std::string response_id = upstream_response_id_from_headers(response_headers);

    if (status >= 400) {
        return make_upstream_http_response(status, std::move(body_bytes),
                                           merge_correlation_headers(response_headers, request_id, response_id));
    }

    usage.pricing_model = billing_model_for_name(*model);
    usage.model_name = *model;
    usage.channel_id = channel_id;
    usage.status_code = status;
    usage.channel_multiplier = channel_price_multiplier(channel_id);
    usage.is_stream = false;
    assign_request_correlation(usage, request_id, response_id);
    if (const auto response_tier = parse_json_string_field(body_bytes, "service_tier"); response_tier.has_value()) {
        usage.service_tier = normalize_usage_service_tier(std::string_view{ *response_tier });
    }
    if (usage.pricing_model != nullptr) {
        parse_billing_request_from_body(usage, GatewayStreamKind::anthropics_messages, body_bytes);
    }

    return make_upstream_http_response(status, std::move(body_bytes),
                                       merge_correlation_headers(response_headers, request_id, response_id));
}

void run_messages_stream(::httplib::Response &res, const ::httplib::Request &req, const GatewayParsedRequest &parsed,
                         std::string_view request_id, long long channel_id, std::string_view client_ip, Request usage,
                         const std::function<void(Request &)> &on_usage)
{
    (void)parsed;
    (void)client_ip;
    const auto model = select_messages_proxy_model(req.body, channel_id);
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

    UpstreamRequest downstream = build_proxy_upstream_request(
        req, "/v1/messages", request_id, req.get_header_value("X-Revlm-Client-Ip"), req.body,
        [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });
    const ScheduledUpstreamStreamExecution executed = open_scheduled_upstream_stream(channel_id, std::move(downstream));
    if (!executed.result.has_value()) {
        apply_http_response(
            http_response(
                502, "Bad Gateway",
                boost::json::object{ { "error", boost::json::object{ { "message", "proxy upstream failed" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
            res);
        return;
    }
    UpstreamStreamResponse upstream = std::move(*executed.result);

    const int status = upstream.status_code;
    const std::string response_id = upstream_response_id_from_headers(upstream.headers);
    const double route_mult = channel_price_multiplier(channel_id);
    usage.pricing_model = billing_model_for_name(*model);
    usage.model_name = *model;
    usage.channel_id = channel_id;
    usage.status_code = status;
    usage.channel_multiplier = route_mult;
    usage.is_stream = true;
    assign_request_correlation(usage, request_id, response_id);

    const std::string requested_service_tier = parse_json_string_field(req.body, "service_tier").value_or("");
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
            if (status >= 400 || !pump.completed || pump.upstream_error || pump.idle_timeout || !pump.saw_usage ||
                u.pricing_model == nullptr) {
                return;
            }
            if (!on_usage) {
                return;
            }
            u.first_token_latency_ms = pump.first_token_latency_ms;
            on_usage(u);
        });
    set_stream_correlation_headers(res, request_id, response_id);
}

} // namespace revlm
