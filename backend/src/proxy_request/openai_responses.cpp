#include "proxy_request/openai_responses.hpp"
#include "channels/channels.hpp"
#include "config/config.hpp"
#include "models/models.hpp"
#include "proxy_request/api_orchestrate.hpp"
#include "proxy_request/gateway_resilience.hpp"
#include "proxy_request/upstream.hpp"
#include "proxy_response/api_stream.hpp"
#include "proxy_response/upstream_http.hpp"
#include "scheduler/scheduler.hpp"
#include "request/request.hpp"
#include "store/database.hpp"
#include "util/json_util.hpp"

#include <boost/json.hpp>
#include <httplib.h>
#include <sys/socket.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

using Clock = std::chrono::steady_clock;

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

std::optional<Model> validate_requested_model(long long channel_id, std::string_view requested_model,
                                              std::string *resolved_model_out)
{
    odb::database &db = database();
    std::string response_id = trim_ascii(requested_model);
    if (response_id.empty()) {
        throw std::invalid_argument("model is required");
    }
    std::string resolved_id = response_id;

    const std::optional<Model> model = billing_model_for_name(resolved_id);
    if (!model.has_value()) {
        return std::nullopt;
    }

    bool allow_openai = false;
    bool allow_anthropic = false;
    {
        ScopedTransaction t(db);
        const auto type_rows = sql_query_rows(
            db, "SELECT DISTINCT c.type FROM channels c WHERE c.id=" + std::to_string(channel_id) + " AND c.status=1");
        t.commit();
        for (const auto &row : type_rows) {
            const int type = static_cast<int>(std::stoll(row[0].value_or("0")));
            if (type == 1 || type == 2) {
                allow_openai = true;
            }
            if (type == 4) {
                allow_anthropic = true;
            }
        }
    }
    const std::string owned = trim_ascii(model->owned_by);
    if (owned == "openai" && !allow_openai) {
        return std::nullopt;
    }
    if (owned == "anthropic" && !allow_anthropic) {
        return std::nullopt;
    }

    if (resolved_model_out != nullptr) {
        *resolved_model_out = resolved_id;
    }
    return model;
}

std::string route_key_hash_for_request(long long channel_id, std::string_view request_id)
{
    return std::to_string(channel_id) + ":" + std::string(request_id);
}

SchedulerConstraints scheduler_constraints_for_model(const Model &model, long long channel_id,
                                                     std::string_view resolved_model, std::string_view)
{
    const SchedulerApi api = model.owned_by == "anthropic" ? SchedulerApi::anthropic : SchedulerApi::openai;
    return build_scheduler_constraints(channel_id, resolved_model, api);
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

Request parse_responses_billing_request(const Model &model, long long user_id, std::string_view body,
                                        std::string_view requested_service_tier, double channel_multiplier)
{
    const std::optional<std::string> response_tier = json_string_value(body, "service_tier");
    const std::string_view effective_tier = response_tier.has_value() ? std::string_view(*response_tier) :
                                                                        requested_service_tier;
    Request req = parse_billing_request_from_body(GatewayStreamKind::openai_responses, model, user_id, body, 1.0,
                                                  channel_multiplier);
    req.tier_multiplier = tier_multiplier_for(model, requested_service_tier, effective_tier, req.input_tokens);
    return req;
}

ProxyUpstreamResponse perform_upstream_request(const SchedulerSelection &selection, std::string_view path,
                                               std::string_view method, std::string_view request_body_json,
                                               std::string_view request_id, std::string_view service_tier)
{
    UpstreamRequest downstream;
    downstream.method = std::string{ method };
    downstream.path = std::string{ path };
    downstream.body = std::string{ request_body_json };
    downstream.headers = {
        { "Content-Type", "application/json" },
        { "Accept", "application/json" },
        { "X-Request-Id", std::string{ request_id } },
    };
    if (!trim_ascii(service_tier).empty()) {
        downstream.headers.push_back({ "X-Revlm-Service-Tier", std::string{ service_tier } });
    }

    const ScheduledUpstreamExecution executed = execute_scheduled_upstream(selection, downstream);
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
        .body = executed.result->response.body,
        .content_type = content_type,
        .sse = is_sse_content_type(content_type),
        .response_id = upstream_response_id_from_headers(executed.result->response.headers),
    };
}

UpstreamSession open_upstream_stream_session(const SchedulerSelection &selection, std::string_view path,
                                             std::string_view method, std::string_view request_body_json,
                                             std::string_view request_id, std::string_view service_tier)
{
    UpstreamRequest downstream;
    downstream.method = std::string{ method };
    downstream.path = std::string{ path };
    downstream.body = std::string{ request_body_json };
    downstream.headers = {
        { "Content-Type", "application/json" },
        { "Accept", "text/event-stream" },
        { "X-Request-Id", std::string{ request_id } },
    };
    if (!trim_ascii(service_tier).empty()) {
        downstream.headers.push_back({ "X-Revlm-Service-Tier", std::string{ service_tier } });
    }

    ScheduledUpstreamStreamExecution executed = open_scheduled_upstream_stream(selection, downstream);
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

ResponsesProxyResult finalize_non_stream_result(std::string_view request_id, const SchedulerSelection &selection,
                                                std::string_view forwarded_model,
                                                std::string_view requested_service_tier,
                                                const ProxyUpstreamResponse &upstream, int latency_ms)
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

    const std::optional<Model> billing_model = billing_model_for_name(forwarded_model);
    if (!billing_model.has_value()) {
        out.response = http_response(502, "Bad Gateway", json_error_body("usage commit failed"),
                                     { { "X-Request-Id", std::string{ request_id } } });
        return out;
    }

    out.has_usage = true;
    out.billable = true;
    out.forwarded_model = std::string{ forwarded_model };
    out.channel_id = selection.channel_id;
    out.status_code = upstream.status;
    out.response_body = upstream.body;
    out.channel_multiplier = selection.route_group_multiplier;
    out.response_id = upstream.response_id;
    out.is_stream = false;
    out.latency_ms = std::max(latency_ms, 0);
    if (const auto response_service_tier = json_string_value(upstream.body, "service_tier");
        response_service_tier.has_value()) {
        out.service_tier = normalize_usage_service_tier(std::string_view(*response_service_tier));
    }
    out.billing_request = parse_responses_billing_request(*billing_model, 0, upstream.body, requested_service_tier,
                                                          selection.route_group_multiplier);
    return out;
}

void stream_upstream_session_to_httplib(::httplib::Response &res, UpstreamSession session, std::string_view request_id,
                                        const Model &billing_model, long long user_id,
                                        std::string_view requested_service_tier, double route_group_multiplier,
                                        std::function<void(int session_status, Request &billing_request,
                                                           const std::optional<std::string> &upstream_response_model,
                                                           long long response_bytes, int first_token_latency_ms)>
                                            on_complete)
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
        std::optional<std::string> upstream_response_model;
        std::string requested_service_tier;
        Model billing_model;
        double channel_multiplier = 1.0;
        long long user_id = 0;
        std::function<void(int, Request &, const std::optional<std::string> &, long long, int)> on_complete;
    };
    auto shared = std::make_shared<Shared>();
    shared->session = std::move(session);
    shared->billing_model = billing_model;
    shared->channel_multiplier = route_group_multiplier;
    shared->user_id = user_id;
    shared->upstream_response_model = json_string_value(shared->session.head.body, "model");
    shared->requested_service_tier = std::string{ requested_service_tier };
    shared->on_complete = std::move(on_complete);

    const int idle_timeout_ms = std::max(1000, config().proxy_upstream_timeout_seconds * 1000);
    res.set_chunked_content_provider(content_type, [shared, idle_timeout_ms](size_t offset,
                                                                             ::httplib::DataSink &sink) mutable {
        if (offset != 0) {
            return false;
        }
        auto stream_gateway =
            make_gateway(GatewayStreamKind::openai_responses, shared->billing_model, 1.0, shared->channel_multiplier);
        const GatewayStreamResult gateway_result = pump_gateway_stream(
            shared->session.stream.read,
            [&sink](std::string_view data) { return sink.write(data.data(), data.size()); }, shared->session.head.body,
            idle_timeout_ms, shared->session.stream.poll_fd, *stream_gateway, shared->user_id);
        if (gateway_result.pump.model.has_value()) {
            shared->upstream_response_model = *gateway_result.pump.model;
        }
        const int session_status = shared->session.head.status;
        shared->session.close_stream();
        if (shared->on_complete && gateway_result.billing_request.has_value()) {
            Request billing_request = *gateway_result.billing_request;
            shared->on_complete(session_status, billing_request, shared->upstream_response_model,
                                static_cast<long long>(gateway_result.pump.response_bytes),
                                gateway_result.pump.first_token_latency_ms);
        }
        sink.done();
        return true;
    });
}

bool stream_upstream_session_to_client(UpstreamSession &session, const ClientWriter &write_client,
                                       std::string_view request_id, Request &billing_request,
                                       std::optional<std::string> &upstream_response_model, long long &response_bytes,
                                       int &first_token_latency_ms)
{
    std::vector<Header> headers{ { "X-Request-Id", std::string{ request_id } } };
    if (!session.head.response_id.empty()) {
        headers.push_back({ "X-Response-Id", session.head.response_id });
    }
    if (!write_client(build_synthetic_stream_response_head(session.head.status, session.head.content_type, headers))) {
        return false;
    }
    auto gateway = make_gateway(GatewayStreamKind::openai_responses, billing_request.model,
                                billing_request.tier_multiplier, billing_request.channel_multiplier);
    const int idle_timeout_ms = std::max(1000, config().proxy_upstream_timeout_seconds * 1000);
    const GatewayStreamResult result = pump_gateway_stream(session.stream.read, write_client, session.head.body,
                                                           idle_timeout_ms, session.stream.poll_fd, *gateway,
                                                           billing_request.user_id);
    if (session.stream.close) {
        session.stream.close();
    }
    response_bytes = static_cast<long long>(result.pump.response_bytes);
    first_token_latency_ms = result.pump.first_token_latency_ms;
    if (result.billing_request.has_value()) {
        billing_request = *result.billing_request;
    }
    if (result.pump.model.has_value()) {
        upstream_response_model = *result.pump.model;
    }
    return !result.pump.upstream_error;
}

} // namespace

ResponsesProxyResult handle_responses_proxy_request(const ::httplib::Request &req, std::string_view method,
                                                    std::string_view path, std::string_view request_id,
                                                    long long channel_id, const ResponsesProxyExecuteOptions &options,
                                                    const std::function<void(ResponsesProxyResult)> &on_stream_usage)
{
    if (method != "POST") {
        return ResponsesProxyResult{
            .response = http_response(405, "Method Not Allowed", json_error_body("method not allowed"),
                                      { { "X-Request-Id", std::string{ request_id } } }),
        };
    }

    const std::string body = transform_request_body(path, req.body);
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
        std::string resolved_model;
        const auto model = validate_requested_model(channel_id, *model_from_body, &resolved_model);
        if (!model.has_value()) {
            return ResponsesProxyResult{
                .response = http_response(404, "Not Found", json_error_body("model is not available"),
                                          { { "X-Request-Id", std::string{ request_id } } }),
            };
        }

        const std::string requested_service_tier =
            normalize_service_tier_request(string_from_request_json(body, "service_tier"));

        ProxyGatewayContext gateway;
        Scheduler &scheduler = gateway.scheduler;
        const SchedulerConstraints constraints =
            scheduler_constraints_for_model(*model, channel_id, resolved_model, path);

        ProxyUpstreamResponse upstream;
        SchedulerSelection selection;
        bool have_upstream = false;

        ClientWriter write_client = options.write_client;
        if (!write_client && options.client_fd >= 0) {
            write_client = client_writer_from_fd(options.client_fd);
        }
        const bool stream_sink_ready = write_client || options.stream_response != nullptr;

        if (stream_requested && stream_sink_ready) {
            UpstreamSession session;
            try {
                selection =
                    scheduler.select(channel_id, route_key_hash_for_request(channel_id, request_id), constraints);
                session =
                    open_upstream_stream_session(selection, path, method, body, request_id, requested_service_tier);
                scheduler.report(selection, scheduler_result_from_upstream_status(session.head.status));
            } catch (const std::exception &) {
                if (selection.channel_id > 0) {
                    SchedulerResult result;
                    result.success = false;
                    result.retriable = true;
                    result.status_code = 503;
                    result.error_class = "network";
                    result.failure_scope = SchedulerFailureScope::channel;
                    scheduler.report(selection, result);
                }
                throw;
            }
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
                const std::optional<Model> billing_model = billing_model_for_name(resolved_model);
                if (!billing_model.has_value()) {
                    throw std::runtime_error("billing model unavailable");
                }
                const int stream_status = session.head.status;
                const std::string stream_response_id = session.head.response_id;
                const std::optional<std::string> head_response_model = json_string_value(session.head.body, "model");
                const std::optional<std::string> head_response_service_tier =
                    json_string_value(session.head.body, "service_tier");
                auto emit_stream_usage = [&](Request &stream_request, int first_token_latency_ms) {
                    if (stream_status >= 400 || !on_stream_usage) {
                        return;
                    }
                    ResponsesProxyResult usage;
                    usage.has_usage = true;
                    usage.billable = true;
                    usage.forwarded_model = resolved_model;
                    usage.channel_id = selection.channel_id;
                    usage.status_code = stream_status;
                    usage.channel_multiplier = selection.route_group_multiplier;
                    usage.response_id = stream_response_id;
                    usage.is_stream = true;
                    usage.latency_ms = elapsed_latency_ms();
                    usage.first_token_latency_ms =
                        std::min(std::max(first_token_latency_ms, 0), std::max(usage.latency_ms, 0));
                    if (head_response_service_tier.has_value()) {
                        usage.service_tier =
                            normalize_usage_service_tier(std::string_view(*head_response_service_tier));
                    }
                    stream_request.channel_multiplier = selection.route_group_multiplier;
                    usage.billing_request = stream_request;
                    on_stream_usage(std::move(usage));
                };
                if (options.stream_response != nullptr) {
                    stream_upstream_session_to_httplib(
                        *options.stream_response, std::move(session), request_id, *billing_model, 0,
                        requested_service_tier, selection.route_group_multiplier,
                        [&](int, Request &stream_request, const std::optional<std::string> &, long long,
                            int first_token_latency_ms) { emit_stream_usage(stream_request, first_token_latency_ms); });
                    HttpResponse stream_response;
                    stream_response.status = stream_status;
                    return ResponsesProxyResult{
                        .response = std::move(stream_response),
                        .handled_stream = true,
                        .stream_status = stream_status,
                    };
                }
                auto billing_request = std::make_unique<Request>(parse_responses_billing_request(
                    *billing_model, 0, session.head.body, requested_service_tier, selection.route_group_multiplier));
                std::optional<std::string> upstream_response_model = head_response_model;
                long long response_bytes = 0;
                int first_token_latency_ms = 0;
                if (!stream_upstream_session_to_client(session, write_client, request_id, *billing_request,
                                                       upstream_response_model, response_bytes,
                                                       first_token_latency_ms)) {
                    throw std::runtime_error("stream pump failed");
                }
                if (session.head.status < 400) {
                    emit_stream_usage(*billing_request, first_token_latency_ms);
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
            try {
                selection =
                    scheduler.select(channel_id, route_key_hash_for_request(channel_id, request_id), constraints);
                upstream = perform_upstream_request(selection, path, method, body, request_id, requested_service_tier);
                scheduler.report(selection, scheduler_result_from_upstream_status(upstream.status));
            } catch (const std::exception &) {
                if (selection.channel_id > 0) {
                    SchedulerResult result;
                    result.success = false;
                    result.retriable = true;
                    result.status_code = 503;
                    result.error_class = "network";
                    result.failure_scope = SchedulerFailureScope::channel;
                    scheduler.report(selection, result);
                }
                throw;
            }
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

        return finalize_non_stream_result(request_id, selection, resolved_model, requested_service_tier, upstream,
                                          elapsed_latency_ms());
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
