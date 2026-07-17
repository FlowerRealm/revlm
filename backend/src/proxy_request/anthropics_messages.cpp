#include "proxy_request/anthropics_messages.hpp"

#include "channels/channels.hpp"
#include "models/models.hpp"
#include "proxy_request/api_orchestrate.hpp"
#include "proxy_request/upstream.hpp"
#include "proxy_response/api_stream.hpp"
#include "proxy_response/upstream_http.hpp"
#include "scheduler/scheduler.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <boost/json.hpp>
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

constexpr int gateway_channel_type_anthropic = 4;

struct MessagesProxySelection {
    Channel channel;
    std::string requested_model;
    std::string forwarded_model;
    double route_group_multiplier = 1.0;
};

std::optional<MessagesProxySelection> select_messages_proxy_target(std::string_view body, long long channel_id)
{
    const auto requested_model = parse_json_string_field(body, "model");
    if (!requested_model.has_value()) {
        return std::nullopt;
    }

    std::string forwarded_model = *requested_model;

    const std::vector<Model> &models = ModelManager::instance().models();
    if (std::ranges::find(models, forwarded_model, &Model::name) == models.end()) {
        return std::nullopt;
    }

    ProxyRoutingDataSource data_source;
    Scheduler scheduler(data_source);
    scheduler.rebuild_routing_snapshot();
    const SchedulerRoutingSnapshot *snapshot = scheduler.routing_snapshot();
    if (snapshot == nullptr) {
        return std::nullopt;
    }
    for (const Channel &channel : snapshot->channels) {
        if (channel.id != channel_id) {
            continue;
        }
        if (!snapshot->channel_supports_model(channel.id, forwarded_model)) {
            return std::nullopt;
        }
        if (channel.type != gateway_channel_type_anthropic || !channel.status || trim_ascii(channel.api_key).empty()) {
            return std::nullopt;
        }
        MessagesProxySelection selection;
        selection.channel = channel;
        selection.requested_model = *requested_model;
        selection.forwarded_model = forwarded_model;
        selection.route_group_multiplier = channel.price_multiplier;
        return selection;
    }

    return std::nullopt;
}

std::string messages_request_body_for_upstream(std::string_view body, const MessagesProxySelection &selection)
{
    if (selection.requested_model == selection.forwarded_model) {
        return std::string{ body };
    }
    return replace_json_string_field(body, "model", selection.forwarded_model);
}

} // namespace

ResponsesProxyResult run_messages_gateway(const ::httplib::Request &req, std::string_view request_id,
                                          long long channel_id)
{
    const auto selection = select_messages_proxy_target(req.body, channel_id);
    if (!selection.has_value()) {
        return ResponsesProxyResult{
            .response = http_response(
                400, "Bad Request",
                boost::json::object{
                    { "error",
                      boost::json::object{ { "message", "messages model unavailable on anthropic channels" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
        };
    }

    const std::string body = messages_request_body_for_upstream(req.body, *selection);
    if (revlm::parse_json_bool_field(body, "stream").value_or(false)) {
        return ResponsesProxyResult{
            .response = http_response(
                400, "Bad Request",
                boost::json::object{
                    { "error", boost::json::object{ { "message", "streaming requires live socket path" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
        };
    }

    SchedulerSelection scheduled{};
    scheduled.channel_id = selection->channel.id;
    scheduled.channel_type = "anthropic";
    scheduled.base_url = selection->channel.base_url;
    scheduled.api_key = selection->channel.api_key;

    const UpstreamRequest downstream = build_proxy_upstream_request(
        req, "/v1/messages", request_id, req.get_header_value("X-Revlm-Client-Ip"), body,
        [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });

    const ScheduledUpstreamExecution executed = execute_scheduled_upstream(scheduled, downstream);
    if (!executed.result.has_value()) {
        return ResponsesProxyResult{
            .response = http_response(
                502, "Bad Gateway",
                boost::json::object{ { "error", boost::json::object{ { "message", "proxy upstream failed" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
        };
    }
    const int status = executed.result->response.status_code;
    std::string body_bytes = executed.result->response.body;
    const std::vector<UpstreamHeader> &response_headers = executed.result->response.headers;
    const std::string response_id = upstream_response_id_from_headers(response_headers);

    if (status >= 400) {
        return ResponsesProxyResult{
            .response = make_upstream_http_response(
                status, std::move(body_bytes), merge_correlation_headers(response_headers, request_id, response_id)),
        };
    }

    ResponsesProxyResult out;
    out.response = make_upstream_http_response(status, body_bytes,
                                               merge_correlation_headers(response_headers, request_id, response_id));
    out.has_usage = true;
    out.billable = true;
    out.forwarded_model = selection->forwarded_model;
    out.channel_id = selection->channel.id;
    out.status_code = status;
    out.response_body = std::move(body_bytes);
    out.channel_multiplier = selection->route_group_multiplier;
    out.response_id = response_id;
    out.is_stream = false;
    if (const auto response_tier = parse_json_string_field(out.response_body, "service_tier");
        response_tier.has_value()) {
        out.service_tier = *response_tier;
    }
    return out;
}

void run_messages_stream(::httplib::Response &res, const ::httplib::Request &req, const GatewayParsedRequest &parsed,
                         std::string_view request_id, long long channel_id, std::string_view client_ip,
                         const std::function<void(ResponsesProxyResult)> &on_stream_usage)
{
    (void)parsed;
    (void)client_ip;
    const auto selection = select_messages_proxy_target(req.body, channel_id);
    if (!selection.has_value()) {
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

    const std::string body = messages_request_body_for_upstream(req.body, *selection);
    SchedulerSelection scheduled{};
    scheduled.channel_id = selection->channel.id;
    scheduled.channel_type = "anthropic";
    scheduled.base_url = selection->channel.base_url;
    scheduled.api_key = selection->channel.api_key;

    const UpstreamRequest downstream = build_proxy_upstream_request(
        req, "/v1/messages", request_id, req.get_header_value("X-Revlm-Client-Ip"), body,
        [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });
    const ScheduledUpstreamStreamExecution executed = open_scheduled_upstream_stream(scheduled, downstream);
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
    const MessagesProxySelection committed = *selection;
    const std::string response_id = upstream_response_id_from_headers(upstream.headers);
    const auto stream_billing_model = billing_model_for_name(committed.forwarded_model);
    const std::string requested_service_tier = parse_json_string_field(req.body, "service_tier").value_or("");
    std::unique_ptr<Gateway> stream_gateway;
    if (stream_billing_model.has_value()) {
        stream_gateway = make_gateway(GatewayStreamKind::anthropics_messages, *stream_billing_model, 1.0,
                                      committed.route_group_multiplier);
    }
    apply_upstream_gateway_stream(res, status, upstream.headers, std::move(upstream), std::move(stream_gateway),
                                  requested_service_tier, 0,
                                  [committed, response_id, status, on_stream_usage](const GatewayStreamResult &result) {
                                      const GatewayStreamPump &pump = result.pump;
                                      if (status >= 400 || !pump.completed || pump.upstream_error ||
                                          pump.idle_timeout || !result.billing_request.has_value()) {
                                          return;
                                      }
                                      if (!on_stream_usage) {
                                          return;
                                      }
                                      ResponsesProxyResult usage;
                                      usage.has_usage = true;
                                      usage.billable = true;
                                      usage.forwarded_model = committed.forwarded_model;
                                      usage.channel_id = committed.channel.id;
                                      usage.status_code = status;
                                      usage.channel_multiplier = committed.route_group_multiplier;
                                      usage.response_id = response_id;
                                      usage.is_stream = true;
                                      usage.first_token_latency_ms = pump.first_token_latency_ms;
                                      usage.billing_request = *result.billing_request;
                                      on_stream_usage(std::move(usage));
                                  });
    set_stream_correlation_headers(res, request_id, response_id);
}

} // namespace revlm
