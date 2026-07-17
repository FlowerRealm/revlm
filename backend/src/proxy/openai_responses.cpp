#include "proxy/openai_responses.hpp"

#include "channels/channels.hpp"
#include "config/config.hpp"
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
#include <boost/json/value.hpp>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <httplib.h>
#include <iostream>
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

} // namespace

void OpenaiResponses::finalize(boost::json::object &json)
{
    const boost::json::object *usage_parent = &json;
    if (const auto *response = json.if_contains("response"); response != nullptr && response->is_object()) {
        usage_parent = &response->as_object();
    }
    const auto *usage_value = usage_parent->if_contains("usage");
    if (usage_value == nullptr || !usage_value->is_object()) {
        return;
    }
    const boost::json::object &usage = usage_value->as_object();
    const long long input_tokens = json_int64_or(usage, "input_tokens");
    const long long output_tokens = json_int64_or(usage, "output_tokens");

    long long cached = json_int64_or(usage, "cache_read_input_tokens");
    if (const auto *details = usage.if_contains("input_tokens_details"); details != nullptr && details->is_object()) {
        const long long from_details = json_int64_or(details->as_object(), "cached_tokens");
        if (from_details > 0) {
            cached = from_details;
        }
    }

    const long long cache_creation_5m = json_int64_or(usage, "cache_creation_input_tokens");
    const long long cache_creation_1h = json_int64_or(usage, "cache_creation_1h_input_tokens");

    request.input_tokens = static_cast<int>(input_tokens);
    request.output_tokens = static_cast<int>(output_tokens);
    request.cache_read_tokens = static_cast<int>(cached);
    request.cache_creation_1h_tokens = static_cast<int>(cache_creation_1h);
    request.cache_creation_5m_tokens = static_cast<int>(cache_creation_5m);
}

namespace
{

using Clock = std::chrono::steady_clock;

double channel_price_multiplier(long long channel_id)
{
    const auto channel = ChannelStore::instance().find_channel(channel_id);
    return channel.has_value() ? channel->price_multiplier : 1.0;
}

struct ProxyOrigin {
    bool https = false;
    std::string host;
    int port = 80;
    std::string base_path;
};

struct ParsedRequestLine {
    std::string_view method;
    std::string_view target;
    std::string_view path;
};

struct ResponseHead {
    int status = 502;
    std::string raw_headers;
    std::string body;
    std::string content_type;
    std::string response_id;
};

struct ProxyUpstreamResponse {
    int status = 502;
    std::string headers;
    std::string body;
    std::string content_type;
    bool sse = false;
    std::string response_id;
};

struct UpstreamSession {
    ResponseHead head;
    UpstreamReadHandle stream;

    void close_stream()
    {
        if (stream.close) {
            stream.close();
        }
    }

    UpstreamSession() = default;
    UpstreamSession(const UpstreamSession &) = delete;
    UpstreamSession &operator=(const UpstreamSession &) = delete;
    UpstreamSession(UpstreamSession &&other) noexcept
        : head(std::move(other.head))
        , stream(std::move(other.stream))
    {
        other.stream = {};
    }
    UpstreamSession &operator=(UpstreamSession &&other) noexcept
    {
        if (this == &other) {
            return *this;
        }
        close_stream();
        head = std::move(other.head);
        stream = std::move(other.stream);
        other.stream = {};
        return *this;
    }
    ~UpstreamSession()
    {
        close_stream();
    }
};

boost::json::value json_error_body(std::string_view message)
{
    return boost::json::object{ { "error", boost::json::object{ { "message", message } } } };
}

std::optional<std::string> model_from_request_json(std::string_view body)
{
    return parse_json_string_field(body, "model");
}

std::optional<bool> bool_from_request_json(std::string_view body, std::string_view field_name)
{
    return parse_json_bool_field(body, field_name);
}

std::optional<std::string> string_from_request_json(std::string_view body, std::string_view field_name)
{
    return parse_json_string_field(body, field_name);
}

std::string normalize_service_tier_request(std::optional<std::string> raw)
{
    if (!raw.has_value()) {
        return {};
    }
    std::string tier = normalize_usage_service_tier(std::string_view(*raw));
    if (tier.empty()) {
        return {};
    }
    if (tier == "auto" || tier == "default" || tier == "flex" || tier == "priority") {
        return tier;
    }
    throw std::invalid_argument("service_tier is unsupported");
}

const Model *validate_requested_model(long long channel_id, std::string_view requested_model)
{
    const std::string model_name = trim_ascii(requested_model);
    if (model_name.empty()) {
        throw std::invalid_argument("model is required");
    }

    const Model *model = billing_model_for_name(model_name);
    if (model == nullptr) {
        return nullptr;
    }

    bool allow_openai = false;
    bool allow_anthropic = false;
    if (const auto channel = ChannelStore::instance().find_channel(channel_id);
        channel.has_value() && channel->status) {
        if (channel->type == 1 || channel->type == 2) {
            allow_openai = true;
        }
        if (channel->type == 4) {
            allow_anthropic = true;
        }
    }
    const std::string owned = trim_ascii(model->owned_by);
    if (owned == "openai" && !allow_openai) {
        return nullptr;
    }
    if (owned == "anthropic" && !allow_anthropic) {
        return nullptr;
    }

    return model;
}

std::string filter_body_for_input_tokens(std::string_view body)
{
    return std::string{ body };
}

std::string transform_request_body(std::string_view path, std::string_view body)
{
    if (path == "/v1/responses/input_tokens") {
        return filter_body_for_input_tokens(body);
    }
    return std::string{ body };
}

std::optional<std::string> json_string_value(std::string_view body, std::string_view field_name)
{
    return parse_json_string_field(body, field_name);
}

double tier_multiplier_for(const Model &model, std::string_view requested_tier, std::string_view response_tier,
                           int input_tokens)
{
    std::string tier = trim_ascii(response_tier.empty() ? requested_tier : response_tier);
    std::transform(tier.begin(), tier.end(), tier.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const bool is_priority = (tier == "priority" || tier == "fast");
    const bool high_context = model.owned_by == "openai" && input_tokens > 272000;
    if (is_priority && high_context) {
        return 4.0;
    }
    if (is_priority || high_context) {
        return 2.0;
    }
    return 1.0;
}

void apply_responses_billing_from_json(Request &usage, const Model *model, std::string_view body,
                                       std::string_view requested_service_tier, double channel_multiplier)
{
    if (model == nullptr) {
        return;
    }
    usage.pricing_model = model;
    usage.model_name = model->name;
    usage.channel_multiplier = channel_multiplier;
    usage.tier_multiplier = 1.0;
    parse_billing_request_from_body(usage, GatewayStreamKind::openai_responses, body);
    const std::optional<std::string> response_tier = json_string_value(body, "service_tier");
    const std::string_view effective_tier = response_tier.has_value() ? std::string_view(*response_tier) :
                                                                        requested_service_tier;
    usage.tier_multiplier = tier_multiplier_for(*model, requested_service_tier, effective_tier, usage.input_tokens);
    if (response_tier.has_value()) {
        usage.service_tier = normalize_usage_service_tier(std::string_view(*response_tier));
    }
}

ProxyUpstreamResponse perform_upstream_request(long long channel_id, std::string_view path, std::string_view method,
                                               std::string request_body_json, std::string_view request_id,
                                               std::string_view service_tier)
{
    UpstreamRequest downstream;
    downstream.method = std::string{ method };
    downstream.path = std::string{ path };
    downstream.body = std::move(request_body_json);
    downstream.headers = {
        { "Content-Type", "application/json" },
        { "Accept", "application/json" },
        { "X-Request-Id", std::string{ request_id } },
    };
    if (!trim_ascii(service_tier).empty()) {
        downstream.headers.push_back({ "X-Revlm-Service-Tier", std::string{ service_tier } });
    }

    const ScheduledUpstreamExecution executed = execute_scheduled_upstream(channel_id, std::move(downstream));
    if (!executed.result.has_value()) {
        throw std::runtime_error(executed.transport_error->message.empty() ? "upstream unavailable" :
                                                                             executed.transport_error->message);
    }

    std::string content_type;
    for (const auto &header : executed.result->response.headers) {
        if (lowercase_ascii(header.name) == "content-type") {
            content_type = trim_ascii(header.value);
            break;
        }
    }
    return ProxyUpstreamResponse{
        .status = executed.result->response.status_code,
        .headers = format_upstream_proxy_response_headers(executed.result->response.status_code,
                                                          executed.result->response.headers,
                                                          executed.result->response.body.size()),
        .body = std::move(executed.result->response.body),
        .content_type = content_type,
        .sse = is_sse_content_type(content_type),
        .response_id = upstream_response_id_from_headers(executed.result->response.headers),
    };
}

UpstreamSession open_upstream_stream_session(long long channel_id, std::string_view path, std::string_view method,
                                             std::string request_body_json, std::string_view request_id,
                                             std::string_view service_tier)
{
    UpstreamRequest downstream;
    downstream.method = std::string{ method };
    downstream.path = std::string{ path };
    downstream.body = std::move(request_body_json);
    downstream.headers = {
        { "Content-Type", "application/json" },
        { "Accept", "text/event-stream" },
        { "X-Request-Id", std::string{ request_id } },
    };
    if (!trim_ascii(service_tier).empty()) {
        downstream.headers.push_back({ "X-Revlm-Service-Tier", std::string{ service_tier } });
    }

    ScheduledUpstreamStreamExecution executed = open_scheduled_upstream_stream(channel_id, std::move(downstream));
    if (!executed.result.has_value()) {
        throw std::runtime_error(executed.transport_error->message.empty() ? "upstream unavailable" :
                                                                             executed.transport_error->message);
    }
    UpstreamStreamResponse stream_response = std::move(*executed.result);

    std::string content_type;
    std::ostringstream raw_headers;
    raw_headers << "HTTP/1.1 " << stream_response.status_code << "\r\n";
    for (const UpstreamHeader &header : stream_response.headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "content-type") {
            content_type = header.value;
        }
        raw_headers << header.name << ": " << header.value << "\r\n";
    }

    UpstreamSession session;
    session.head.status = stream_response.status_code;
    session.head.raw_headers = raw_headers.str();
    session.head.body = std::move(stream_response.initial_body);
    session.head.content_type = content_type;
    session.head.response_id = upstream_response_id_from_headers(stream_response.headers);
    session.stream = std::move(stream_response.stream);
    return session;
}

ResponsesProxyResult finalize_non_stream_result(std::string_view request_id, long long channel_id,
                                                std::string_view model_name, std::string_view requested_service_tier,
                                                const ProxyUpstreamResponse &upstream, int latency_ms, Request &usage)
{
    ResponsesProxyResult out;
    out.response.status = upstream.status;
    out.response.reason = (upstream.status >= 200 && upstream.status < 300) ? "OK" : "Bad Gateway";
    if (auto parsed = parse_json(upstream.body)) {
        out.response.body = std::move(*parsed);
    } else {
        out.response.body = boost::json::value(upstream.body);
    }
    out.response.content_type = upstream.content_type.empty() ? "application/json; charset=utf-8" :
                                                                upstream.content_type;
    out.response.headers.push_back({ "X-Request-Id", std::string{ request_id } });
    if (!upstream.response_id.empty()) {
        out.response.headers.push_back({ "X-Response-Id", upstream.response_id });
    }

    if (upstream.status >= 400) {
        return out;
    }

    const Model *billing_model = billing_model_for_name(model_name);
    if (billing_model == nullptr) {
        out.response = http_response(502, "Bad Gateway", json_error_body("usage commit failed"),
                                     { { "X-Request-Id", std::string{ request_id } } });
        return out;
    }

    usage.channel_id = channel_id;
    usage.status_code = upstream.status;
    usage.is_stream = false;
    usage.latency_ms = std::max(latency_ms, 0);
    assign_request_correlation(usage, request_id, upstream.response_id);
    apply_responses_billing_from_json(usage, billing_model, upstream.body, requested_service_tier,
                                      channel_price_multiplier(channel_id));
    return out;
}

void stream_upstream_session_to_httplib(::httplib::Response &res, UpstreamSession session, std::string_view request_id,
                                        Request usage, std::string_view requested_service_tier,
                                        double route_group_multiplier,
                                        std::function<void(Request &usage, int first_token_latency_ms)> on_complete)
{
    const int stream_status = session.head.status;
    const std::string content_type = session.head.content_type.empty() ? "text/event-stream; charset=utf-8" :
                                                                         session.head.content_type;
    res.status = stream_status;
    res.set_header("Content-Type", content_type);
    res.set_header("X-Request-Id", std::string(request_id));
    if (!session.head.response_id.empty()) {
        res.set_header("X-Response-Id", session.head.response_id);
    }

    struct Shared {
        UpstreamSession session;
        Request usage;
        std::string requested_service_tier;
        double channel_multiplier = 1.0;
        std::function<void(Request &, int)> on_complete;
    };
    auto shared = std::make_shared<Shared>();
    shared->session = std::move(session);
    shared->usage = std::move(usage);
    shared->usage.channel_multiplier = route_group_multiplier;
    shared->channel_multiplier = route_group_multiplier;
    shared->requested_service_tier = std::string{ requested_service_tier };
    shared->on_complete = std::move(on_complete);

    const int idle_timeout_ms = std::max(1000, config().proxy_upstream_timeout_seconds * 1000);
    res.set_chunked_content_provider(content_type, [shared, idle_timeout_ms](size_t offset,
                                                                             ::httplib::DataSink &sink) mutable {
        if (offset != 0) {
            return false;
        }
        auto stream_gateway = make_gateway(GatewayStreamKind::openai_responses, shared->usage.pricing_model, 1.0,
                                           shared->channel_multiplier, shared->usage);
        const GatewayStreamResult gateway_result = pump_gateway_stream(
            shared->session.stream.read,
            [&sink](std::string_view data) { return sink.write(data.data(), data.size()); }, shared->session.head.body,
            idle_timeout_ms, shared->session.stream.poll_fd, *stream_gateway);
        const int session_status = shared->session.head.status;
        shared->session.close_stream();
        if (shared->on_complete && gateway_result.pump.saw_usage && session_status < 400) {
            if (!shared->requested_service_tier.empty() && shared->usage.service_tier.null()) {
                shared->usage.service_tier =
                    normalize_usage_service_tier(std::string_view{ shared->requested_service_tier });
            }
            shared->on_complete(shared->usage, gateway_result.pump.first_token_latency_ms);
        }
        sink.done();
        return true;
    });
}

bool stream_upstream_session_to_client(UpstreamSession &session, const ClientWriter &write_client,
                                       std::string_view request_id, Request &usage,
                                       std::optional<std::string> &upstream_response_model, long long &response_bytes,
                                       int &first_token_latency_ms, bool &had_usage_out)
{
    std::vector<Header> headers{ { "X-Request-Id", std::string{ request_id } } };
    if (!session.head.response_id.empty()) {
        headers.push_back({ "X-Response-Id", session.head.response_id });
    }
    if (!write_client(build_synthetic_stream_response_head(session.head.status, session.head.content_type, headers))) {
        return false;
    }
    auto gateway = make_gateway(GatewayStreamKind::openai_responses, usage.pricing_model, usage.tier_multiplier,
                                usage.channel_multiplier, usage);
    const int idle_timeout_ms = std::max(1000, config().proxy_upstream_timeout_seconds * 1000);
    const GatewayStreamResult result = pump_gateway_stream(session.stream.read, write_client, session.head.body,
                                                           idle_timeout_ms, session.stream.poll_fd, *gateway);
    if (session.stream.close) {
        session.stream.close();
    }
    response_bytes = static_cast<long long>(result.pump.response_bytes);
    first_token_latency_ms = result.pump.first_token_latency_ms;
    had_usage_out = result.pump.saw_usage;
    if (result.pump.model.has_value()) {
        upstream_response_model = *result.pump.model;
    }
    return !result.pump.upstream_error;
}

} // namespace

ResponsesProxyResult handle_responses_proxy_request(const ::httplib::Request &req, std::string_view method,
                                                    std::string_view path, std::string_view request_id,
                                                    long long channel_id, Request &usage,
                                                    const ResponsesProxyExecuteOptions &options)
{
    if (method != "POST") {
        return ResponsesProxyResult{
            .response = http_response(405, "Method Not Allowed", json_error_body("method not allowed"),
                                      { { "X-Request-Id", std::string{ request_id } } }),
        };
    }

    std::string body = transform_request_body(path, req.body);
    const bool stream_requested = bool_from_request_json(body, "stream").value_or(false);
    const std::optional<std::string> model_from_body = model_from_request_json(body);
    if (!model_from_body.has_value()) {
        return ResponsesProxyResult{
            .response = http_response(400, "Bad Request", json_error_body("model is required"),
                                      { { "X-Request-Id", std::string{ request_id } } }),
        };
    }

    const auto request_started_at = Clock::now();
    const auto elapsed_latency_ms = [&request_started_at]() {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - request_started_at).count());
    };

    try {
        const Model *model = validate_requested_model(channel_id, *model_from_body);
        if (model == nullptr) {
            return ResponsesProxyResult{
                .response = http_response(404, "Not Found", json_error_body("model is not available"),
                                          { { "X-Request-Id", std::string{ request_id } } }),
            };
        }

        const std::string requested_service_tier =
            normalize_service_tier_request(string_from_request_json(body, "service_tier"));

        ProxyUpstreamResponse upstream;
        bool have_upstream = false;
        const double route_mult = channel_price_multiplier(channel_id);

        ClientWriter write_client = options.write_client;
        if (!write_client && options.client_fd >= 0) {
            write_client = client_writer_from_fd(options.client_fd);
        }
        const bool stream_sink_ready = write_client || options.stream_response != nullptr;

        if (stream_requested && stream_sink_ready) {
            UpstreamSession session =
                open_upstream_stream_session(channel_id, path, method, body, request_id, requested_service_tier);
            if (!is_sse_content_type(session.head.content_type)) {
                upstream = ProxyUpstreamResponse{
                    .status = session.head.status,
                    .headers = session.head.raw_headers,
                    .body = session.head.body + read_remaining_stream(session.stream),
                    .content_type = session.head.content_type,
                    .sse = false,
                    .response_id = session.head.response_id,
                };
                have_upstream = true;
            } else {
                const Model *billing_model = model;
                const int stream_status = session.head.status;
                const std::string stream_response_id = session.head.response_id;
                const std::optional<std::string> head_response_service_tier =
                    json_string_value(session.head.body, "service_tier");

                usage.pricing_model = billing_model;
                usage.model_name = model->name;
                usage.channel_id = channel_id;
                usage.status_code = stream_status;
                usage.channel_multiplier = route_mult;
                usage.is_stream = true;
                assign_request_correlation(usage, request_id, stream_response_id);
                if (head_response_service_tier.has_value()) {
                    usage.service_tier = normalize_usage_service_tier(std::string_view(*head_response_service_tier));
                }

                auto emit_stream_usage = [&](Request &stream_request, int first_token_latency_ms) {
                    if (stream_status >= 400 || !options.on_usage || stream_request.pricing_model == nullptr) {
                        return;
                    }
                    stream_request.latency_ms = elapsed_latency_ms();
                    stream_request.first_token_latency_ms =
                        std::min(std::max(first_token_latency_ms, 0), std::max(stream_request.latency_ms, 0));
                    stream_request.channel_multiplier = route_mult;
                    options.on_usage(stream_request);
                };
                if (options.stream_response != nullptr) {
                    Request stream_usage = usage;
                    stream_upstream_session_to_httplib(*options.stream_response, std::move(session), request_id,
                                                       std::move(stream_usage), requested_service_tier, route_mult,
                                                       [&](Request &stream_request, int first_token_latency_ms) {
                                                           emit_stream_usage(stream_request, first_token_latency_ms);
                                                       });
                    HttpResponse stream_response;
                    stream_response.status = stream_status;
                    return ResponsesProxyResult{
                        .response = std::move(stream_response),
                        .handled_stream = true,
                        .stream_status = stream_status,
                    };
                }
                apply_responses_billing_from_json(usage, billing_model, session.head.body, requested_service_tier,
                                                  route_mult);
                std::optional<std::string> upstream_response_model = json_string_value(session.head.body, "model");
                long long response_bytes = 0;
                int first_token_latency_ms = 0;
                bool had_usage = false;
                if (!stream_upstream_session_to_client(session, write_client, request_id, usage,
                                                       upstream_response_model, response_bytes, first_token_latency_ms,
                                                       had_usage)) {
                    throw std::runtime_error("stream pump failed");
                }
                (void)had_usage;
                if (session.head.status < 400 && usage.pricing_model != nullptr) {
                    usage.latency_ms = elapsed_latency_ms();
                    usage.first_token_latency_ms =
                        std::min(std::max(first_token_latency_ms, 0), std::max(usage.latency_ms, 0));
                    if (!commit_proxy_usage(usage)) {
                        throw std::runtime_error("usage commit failed");
                    }
                }
                HttpResponse stream_response;
                stream_response.status = session.head.status;
                return ResponsesProxyResult{
                    .response = std::move(stream_response),
                    .handled_stream = true,
                    .stream_status = session.head.status,
                };
            }
        }

        if (!have_upstream) {
            upstream = perform_upstream_request(channel_id, path, method, body, request_id, requested_service_tier);
        }

        if (path == "/v1/responses/input_tokens") {
            HttpResponse response;
            response.status = upstream.status;
            response.reason = (upstream.status >= 200 && upstream.status < 300) ? "OK" : "Bad Gateway";
            if (auto parsed = parse_json(upstream.body)) {
                response.body = std::move(*parsed);
            } else {
                response.body = boost::json::value(upstream.body);
            }
            response.content_type = upstream.content_type.empty() ? "application/json; charset=utf-8" :
                                                                    upstream.content_type;
            response.headers.push_back({ "X-Request-Id", std::string{ request_id } });
            if (!upstream.response_id.empty()) {
                response.headers.push_back({ "X-Response-Id", upstream.response_id });
            }
            return ResponsesProxyResult{
                .response = std::move(response),
            };
        }

        return finalize_non_stream_result(request_id, channel_id, model->name, requested_service_tier, upstream,
                                          elapsed_latency_ms(), usage);
    } catch (const std::invalid_argument &err) {
        return ResponsesProxyResult{
            .response = http_response(400, "Bad Request", json_error_body(err.what()),
                                      { { "X-Request-Id", std::string{ request_id } } }),
        };
    } catch (const std::exception &err) {
        return ResponsesProxyResult{
            .response = http_response(502, "Bad Gateway", json_error_body(err.what()),
                                      { { "X-Request-Id", std::string{ request_id } } }),
        };
    }
}

} // namespace revlm
