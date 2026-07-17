#include "proxy_request/api_orchestrate.hpp"

#include "auth/security.hpp"
#include "channels/channels.hpp"
#include "config/config.hpp"
#include "models/models.hpp"
#include "models/quota.hpp"
#include "proxy_request/gateway_resilience.hpp"
#include "proxy_request/upstream.hpp"
#include "request/request.hpp"
#include "server/http_server.hpp"
#include "users/users.hpp"
#include "util/strings.hpp"

#include <algorithm>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>
#include <exception>
#include <functional>
#include <httplib.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revlm
{

void apply_http_response(const HttpResponse &response, ::httplib::Response &res)
{
    res.status = response.status;
    if (!response.reason.empty()) {
        res.reason = response.reason;
    }
    for (const Header &header : response.headers) {
        res.set_header(header.name, header.value);
    }
    res.set_content(boost::json::serialize(response.body), response.content_type);
}

std::string upstream_response_id_from_headers(const std::vector<UpstreamHeader> &headers)
{
    std::string fallback;
    for (const UpstreamHeader &header : headers) {
        const std::string lower = lowercase_ascii(header.name);
        const std::string value = trim_ascii(header.value);
        if (value.empty()) {
            continue;
        }
        if (lower == "x-request-id") {
            return value;
        }
        if (fallback.empty() && lower == "request-id") {
            fallback = value;
        }
    }
    return fallback;
}

void assign_request_correlation(Request &request, std::string_view request_id, std::string_view response_id)
{
    request.request_id = std::string{ request_id };
    if (!response_id.empty()) {
        request.response_id = std::string{ response_id };
    }
}

void set_stream_correlation_headers(::httplib::Response &res, std::string_view request_id, std::string_view response_id)
{
    res.set_header("X-Request-Id", std::string{ request_id });
    if (!response_id.empty()) {
        res.set_header("X-Response-Id", std::string{ response_id });
    }
}

std::vector<Header> merge_correlation_headers(const std::vector<UpstreamHeader> &upstream_headers,
                                              std::string_view request_id, std::string_view response_id)
{
    std::vector<Header> headers;
    headers.reserve(upstream_headers.size() + 2);
    for (const UpstreamHeader &header : upstream_headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "x-request-id" || lower == "x-response-id") {
            continue;
        }
        headers.push_back({ header.name, header.value });
    }
    headers.push_back({ "X-Request-Id", std::string{ request_id } });
    if (!response_id.empty()) {
        headers.push_back({ "X-Response-Id", std::string{ response_id } });
    }
    return headers;
}

const Model *billing_model_for_name(std::string_view name)
{
    const std::vector<Model> &models = ModelManager::instance().models();
    const auto it = std::ranges::find(models, name, &Model::name);
    if (it == models.end()) {
        return nullptr;
    }
    return &(*it);
}

std::optional<HttpResponse> paygo_balance_gate(long long user_id, std::string_view request_id)
{
    if (UserStore::instance().has_positive_user_balance(user_id)) {
        return std::nullopt;
    }
    return http_response(
        402, "Payment Required",
        boost::json::object{ { "error", boost::json::object{ { "message", "insufficient balance" } } } },
        { { "X-Request-Id", std::string{ request_id } } });
}

bool commit_proxy_usage(Request &usage_request)
{
    if (usage_request.id <= 0) {
        return false;
    }
    Quota().charge(usage_request);
    return usage_request.commit(request_timestamp_now());
}

ScheduledUpstreamExecution execute_scheduled_upstream(long long channel_id, UpstreamRequest downstream)
{
    UpstreamExecutor executor;
    try {
        const int timeout_ms = config().proxy_upstream_timeout_seconds * 1000;
        const auto channel = ChannelStore::instance().find_channel(channel_id);
        if (!channel.has_value()) {
            throw std::runtime_error("channel not found");
        }
        const bool allow_private_target = upstream_channel_allows_private_target(channel->base_url);
        UpstreamExecutionResult executed = execute_with_default_transport(executor, channel_id, std::move(downstream),
                                                                          timeout_ms, allow_private_target);
        return ScheduledUpstreamExecution{
            .result = std::move(executed),
            .transport_error = std::nullopt,
        };
    } catch (const std::invalid_argument &) {
        return ScheduledUpstreamExecution{
            .result = std::nullopt,
            .transport_error =
                GatewayAttemptTransportError{
                    .stage = "parse",
                    .message = "upstream URL is invalid",
                },
        };
    } catch (const std::exception &) {
        return ScheduledUpstreamExecution{
            .result = std::nullopt,
            .transport_error =
                GatewayAttemptTransportError{
                    .stage = "connect",
                    .message = "upstream connect failed",
                },
        };
    }
}

ScheduledUpstreamStreamExecution open_scheduled_upstream_stream(long long channel_id, UpstreamRequest downstream)
{
    UpstreamExecutor executor;
    try {
        const int timeout_ms = config().proxy_upstream_timeout_seconds * 1000;
        const auto channel = ChannelStore::instance().find_channel(channel_id);
        if (!channel.has_value()) {
            throw std::runtime_error("channel not found");
        }
        const bool allow_private_target = upstream_channel_allows_private_target(channel->base_url);
        const UpstreamPreparedRequest prepared =
            executor.prepare(channel_id, std::move(downstream), false, !allow_private_target);
        UpstreamStreamResponse upstream =
            default_upstream_http_stream_transport(prepared, timeout_ms, allow_private_target);
        return ScheduledUpstreamStreamExecution{
            .result = std::move(upstream),
            .transport_error = std::nullopt,
        };
    } catch (const std::invalid_argument &) {
        return ScheduledUpstreamStreamExecution{
            .result = std::nullopt,
            .transport_error =
                GatewayAttemptTransportError{
                    .stage = "parse",
                    .message = "upstream URL is invalid",
                },
        };
    } catch (const std::exception &) {
        return ScheduledUpstreamStreamExecution{
            .result = std::nullopt,
            .transport_error =
                GatewayAttemptTransportError{
                    .stage = "connect",
                    .message = "upstream connect failed",
                },
        };
    }
}

std::string remove_json_field(std::string_view json, std::string_view field_name)
{
    boost::system::error_code ec;
    boost::json::value value = boost::json::parse(json, ec);
    if (ec || !value.is_object()) {
        return std::string{ json };
    }
    value.as_object().erase(field_name);
    return boost::json::serialize(value);
}

std::vector<UpstreamHeader> proxy_forward_headers(const ::httplib::Request &req, std::string_view request_id,
                                                  std::string_view client_ip,
                                                  std::function<bool(std::string_view)> drop_header)
{
    std::string original_host = req.get_header_value("Host");
    std::string forwarded_proto = "http";
    if (is_trusted_proxy_ipv4(client_ip, default_trusted_proxies())) {
        if (const auto host = trusted_forwarded_host(req.get_header_value("X-Forwarded-Host")); host.has_value()) {
            original_host = *host;
        }
        if (const auto proto = trusted_forwarded_proto(req.get_header_value("X-Forwarded-Proto")); proto.has_value()) {
            forwarded_proto = *proto;
        }
    }

    std::vector<UpstreamHeader> headers;
    headers.push_back({ "X-Request-Id", std::string{ request_id } });
    headers.push_back({ "X-Forwarded-Proto", forwarded_proto });
    if (!original_host.empty()) {
        headers.push_back({ "X-Forwarded-Host", original_host });
    }
    if (!client_ip.empty()) {
        headers.push_back({ "X-Forwarded-For", std::string{ client_ip } });
    }
    for (const auto &header : req.headers) {
        const std::string lower = lowercase_ascii(header.first);
        if (is_hop_by_hop_header(header.first) || lower == "host" || lower == "connection" ||
            lower == "content-length") {
            continue;
        }
        if (drop_header && !drop_header(lower)) {
            continue;
        }
        headers.push_back({ header.first, header.second });
    }
    return headers;
}

UpstreamRequest build_proxy_upstream_request(const ::httplib::Request &req, std::string_view path,
                                             std::string_view request_id, std::string_view client_ip, std::string body,
                                             std::function<bool(std::string_view)> drop_header)
{
    UpstreamRequest downstream;
    downstream.method = "POST";
    downstream.path = std::string{ path };
    downstream.body = std::move(body);
    downstream.headers = proxy_forward_headers(req, request_id, client_ip, std::move(drop_header));
    return downstream;
}

} // namespace revlm
