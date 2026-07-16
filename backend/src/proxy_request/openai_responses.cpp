#include "proxy_request/openai_responses.hpp"

#include "channels/channels.hpp"
#include "config/config.hpp"
#include "models/models.hpp"
#include "proxy_request/api_orchestrate.hpp"
#include "proxy_request/gateway_resilience.hpp"
#include "proxy_request/http_client.hpp"
#include "proxy_request/token_auth.hpp"
#include "proxy_request/upstream.hpp"
#include "proxy_response/api_stream.hpp"
#include "proxy_response/upstream_http.hpp"
#include "scheduler/scheduler.hpp"
#include "request/request.hpp"
#include "store/database.hpp"
#include "util/json_util.hpp"
#include "util/user_input.hpp"

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

std::string_view request_body(std::string_view request)
{
    const size_t body_start = request.find("\r\n\r\n");
    if (body_start == std::string_view::npos) {
        return {};
    }
    return request.substr(body_start + 4);
}

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

std::optional<Model> validate_requested_model(const TokenAuth &auth, std::string_view requested_model,
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
        const auto type_rows = sql_query_rows(db, "SELECT DISTINCT c.type FROM channels c WHERE c.id=" +
                                                      std::to_string(auth.channel_id) + " AND c.status=1");
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

std::string route_key_hash_for_request(const TokenAuth &auth, std::string_view request_id)
{
    return std::to_string(auth.user_id) + ":" + std::string(request_id);
}

SchedulerConstraints scheduler_constraints_for_model(const Model &model, const TokenAuth &auth,
                                                     std::string_view resolved_model, std::string_view)
{
    const SchedulerApi api = model.owned_by == "anthropic" ? SchedulerApi::anthropic : SchedulerApi::openai;
    return build_scheduler_constraints(auth.channel_id, resolved_model, api);
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

HttpResponse finalize_non_stream_usage(std::string_view request_id, long long usage_event_id, const TokenAuth &auth,
                                       const SchedulerSelection &selection, std::string_view endpoint_path,
                                       std::string_view forwarded_model, std::string_view requested_service_tier,
                                       const ProxyUpstreamResponse &upstream, int latency_ms)
{
    HttpResponse response;
    response.status = upstream.status;
    response.reason = (upstream.status >= 200 && upstream.status < 300) ? "OK" : "Bad Gateway";
    if (auto parsed = parse_json(upstream.body)) {
        response.body = std::move(*parsed);
    } else {
        response.body = boost::json::value(upstream.body);
    }
    response.content_type = upstream.content_type.empty() ? "application/json; charset=utf-8" : upstream.content_type;
    response.headers.push_back({ "X-Request-Id", std::string{ request_id } });
    if (!upstream.response_id.empty()) {
        response.headers.push_back({ "X-Response-Id", upstream.response_id });
    }

    if (upstream.status >= 400) {
        return response;
    }

    if (usage_event_id <= 0) {
        return http_response(502, "Bad Gateway", json_error_body("usage commit failed"),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    const std::optional<Model> billing_model = billing_model_for_name(forwarded_model);
    if (!billing_model.has_value()) {
        return http_response(502, "Bad Gateway", json_error_body("usage commit failed"),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
    auto billing_request = std::make_unique<Request>(parse_responses_billing_request(
        *billing_model, auth.user_id, upstream.body, requested_service_tier, selection.route_group_multiplier));
    const std::optional<std::string> response_service_tier = json_string_value(upstream.body, "service_tier");

    Request usage_request = make_proxy_usage_request(auth, forwarded_model, endpoint_path, usage_event_id,
                                                     selection.channel_id, upstream.status, false);
    assign_request_correlation(usage_request, request_id, upstream.response_id);
    if (response_service_tier.has_value()) {
        usage_request.service_tier = normalize_usage_service_tier(std::string_view(*response_service_tier));
    }
    usage_request.latency_ms = std::max(latency_ms, 0);

    if (!commit_proxy_usage(usage_request, billing_request.get())) {
        return http_response(502, "Bad Gateway", json_error_body("usage commit failed"),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    return response;
}

bool commit_stream_usage(std::string_view request_id, long long usage_event_id, std::string_view response_id,
                         const TokenAuth &auth, const SchedulerSelection &selection, std::string_view endpoint_path,
                         std::string_view forwarded_model, Request &billing_request,
                         const std::optional<std::string> &response_service_tier, int status_code, int latency_ms,
                         int first_token_latency_ms)
{
    if (status_code >= 400) {
        return true;
    }
    if (usage_event_id <= 0) {
        std::cerr << "stream usage commit failed: invalid usage event id: " << request_id << '\n';
        return false;
    }

    billing_request.channel_multiplier = selection.route_group_multiplier;
    Request usage_request = make_proxy_usage_request(auth, forwarded_model, endpoint_path, usage_event_id,
                                                     selection.channel_id, status_code, true);
    assign_request_correlation(usage_request, request_id, response_id);
    if (response_service_tier.has_value()) {
        usage_request.service_tier = normalize_usage_service_tier(std::string_view(*response_service_tier));
    }
    usage_request.latency_ms = std::max(latency_ms, 0);
    usage_request.first_token_latency_ms = std::min(std::max(first_token_latency_ms, 0), usage_request.latency_ms);
    if (!commit_proxy_usage(usage_request, &billing_request)) {
        std::cerr << "stream usage commit failed: " << request_id << '\n';
        return false;
    }
    return true;
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

struct CompactGatewaySelection {
    TokenAuth auth;
    std::string requested_model;
    std::string forwarded_model;
    double route_group_multiplier = 1.0;
};

std::optional<CompactGatewaySelection> select_compact_gateway_target(std::string_view body, const TokenAuth &auth)
{
    const auto requested_model = parse_json_string_field(body, "model");
    if (!requested_model.has_value()) {
        return std::nullopt;
    }

    std::string mapped_model = *requested_model;

    const std::vector<Model> &models = ModelManager::instance().models();
    if (std::ranges::find(models, mapped_model, &Model::name) == models.end()) {
        return std::nullopt;
    }

    CompactGatewaySelection selection;
    selection.auth = auth;
    selection.requested_model = *requested_model;
    selection.forwarded_model = mapped_model;
    selection.route_group_multiplier = 1.0;
    for (const Channel &channel : ChannelStore::instance().list_channels()) {
        if (channel.id == auth.channel_id) {
            selection.route_group_multiplier = channel.price_multiplier;
            break;
        }
    }
    return selection;
}

std::string compact_request_body_for_gateway(std::string_view body, const CompactGatewaySelection &selection)
{
    std::string out = remove_json_field(body, "session_id");
    if (selection.requested_model != selection.forwarded_model) {
        out = replace_json_string_field(out, "model", selection.forwarded_model);
    }
    return out;
}

} // namespace

ResponsesProxyResult handle_responses_proxy_request(std::string_view raw_request, std::string_view method,
                                                    std::string_view path, std::string_view request_id,
                                                    long long usage_event_id,
                                                    const ResponsesProxyExecuteOptions &options)
{
    if (method != "POST") {
        return ResponsesProxyResult{
            .response = http_response(405, "Method Not Allowed", json_error_body("method not allowed"),
                                      { { "X-Request-Id", std::string{ request_id } } }),
        };
    }

    const RequireProxyAuthResult auth_gate = require_proxy_auth(authenticated_token(raw_request), request_id);
    if (!auth_gate.auth.has_value()) {
        return ResponsesProxyResult{ .response = *auth_gate.error };
    }
    const TokenAuth &auth = *auth_gate.auth;

    const std::string body = transform_request_body(path, request_body(raw_request));
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
        const auto model = validate_requested_model(auth, *model_from_body, &resolved_model);
        if (!model.has_value()) {
            return ResponsesProxyResult{
                .response = http_response(404, "Not Found", json_error_body("model is not available"),
                                          { { "X-Request-Id", std::string{ request_id } } }),
            };
        }
        if (const auto quota_error = paygo_balance_gate(auth.user_id, request_id); quota_error.has_value()) {
            return ResponsesProxyResult{ .response = *quota_error };
        }

        const std::string requested_service_tier =
            normalize_service_tier_request(string_from_request_json(body, "service_tier"));

        ProxyGatewayContext gateway;
        Scheduler &scheduler = gateway.scheduler;
        const SchedulerConstraints constraints = scheduler_constraints_for_model(*model, auth, resolved_model, path);

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
                selection = scheduler.select(auth.user_id, route_key_hash_for_request(auth, request_id), constraints);
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
                if (options.stream_response != nullptr) {
                    stream_upstream_session_to_httplib(
                        *options.stream_response, std::move(session), request_id, *billing_model, auth.user_id,
                        requested_service_tier, selection.route_group_multiplier,
                        [&](int, Request &stream_request, const std::optional<std::string> &, long long,
                            int first_token_latency_ms) {
                            if (stream_status >= 400) {
                                return;
                            }
                            if (!commit_stream_usage(request_id, usage_event_id, stream_response_id, auth, selection,
                                                     path, resolved_model, stream_request, head_response_service_tier,
                                                     stream_status, elapsed_latency_ms(), first_token_latency_ms)) {
                                std::cerr << "stream usage commit failed: " << request_id << '\n';
                            }
                        });
                    HttpResponse stream_response;
                    stream_response.status = stream_status;
                    return ResponsesProxyResult{
                        .response = std::move(stream_response),
                        .handled_stream = true,
                        .stream_status = stream_status,
                    };
                }
                auto billing_request = std::make_unique<Request>(
                    parse_responses_billing_request(*billing_model, auth.user_id, session.head.body,
                                                    requested_service_tier, selection.route_group_multiplier));
                std::optional<std::string> upstream_response_model = head_response_model;
                long long response_bytes = 0;
                int first_token_latency_ms = 0;
                if (!stream_upstream_session_to_client(session, write_client, request_id, *billing_request,
                                                       upstream_response_model, response_bytes,
                                                       first_token_latency_ms)) {
                    throw std::runtime_error("stream pump failed");
                }
                if (session.head.status < 400) {
                    if (!commit_stream_usage(request_id, usage_event_id, stream_response_id, auth, selection, path,
                                             resolved_model, *billing_request, head_response_service_tier,
                                             session.head.status, elapsed_latency_ms(), first_token_latency_ms)) {
                        std::cerr << "stream usage commit failed: " << request_id << '\n';
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
            try {
                selection = scheduler.select(auth.user_id, route_key_hash_for_request(auth, request_id), constraints);
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

        return ResponsesProxyResult{
            .response = finalize_non_stream_usage(request_id, usage_event_id, auth, selection, path, resolved_model,
                                                  requested_service_tier, upstream, elapsed_latency_ms()),
        };
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

HttpResponse run_responses_compact_gateway(const ::httplib::Request &req, std::string_view request_id,
                                           long long usage_event_id)
{
    const RequireProxyAuthResult auth_gate = require_proxy_auth(authenticated_token(req), request_id);
    if (!auth_gate.auth.has_value()) {
        return *auth_gate.error;
    }
    const TokenAuth &auth = *auth_gate.auth;
    if (trim_ascii(config().compact_gateway_base_url).empty() || trim_ascii(config().compact_gateway_key).empty()) {
        return http_response(
            502, "Bad Gateway",
            boost::json::object{ { "error", boost::json::object{ { "message", "compact gateway unavailable" } } } },
            { { "X-Request-Id", std::string{ request_id } } });
    }
    const auto selection = select_compact_gateway_target(req.body, auth);
    if (!selection.has_value()) {
        return http_response(
            400, "Bad Request",
            boost::json::object{ { "error", boost::json::object{ { "message", "compact model unavailable" } } } },
            { { "X-Request-Id", std::string{ request_id } } });
    }

    if (revlm::parse_json_bool_field(req.body, "stream").value_or(false)) {
        return http_response(
            400, "Bad Request",
            boost::json::object{
                { "error", boost::json::object{ { "message", "streaming requires live socket path" } } } },
            { { "X-Request-Id", std::string{ request_id } } });
    }

    if (const auto quota_error = paygo_balance_gate(auth.user_id, request_id); quota_error.has_value()) {
        return *quota_error;
    }

    const std::string body = compact_request_body_for_gateway(req.body, *selection);
    UpstreamPreparedRequest prepared;
    prepared.base_url = validate_upstream_base_url(config().compact_gateway_base_url);
    enforce_compact_gateway_guard(prepared.base_url);
    prepared.method = "POST";
    prepared.url = build_upstream_url(prepared.base_url, "/v1/responses/compact", "");
    const std::string content_type = trim_ascii(req.get_header_value("Content-Type"));
    prepared.headers = {
        { "Content-Type", content_type.empty() ? "application/json" : content_type },
        { "Authorization", "Bearer " + trim_ascii(config().compact_gateway_key) },
        { "X-Request-Id", std::string{ request_id } },
    };
    const std::string session_id = trim_ascii(req.get_header_value("session_id"));
    if (!session_id.empty()) {
        prepared.headers.push_back({ "session_id", session_id });
    }
    const std::string originator = trim_ascii(req.get_header_value("originator"));
    if (!originator.empty()) {
        prepared.headers.push_back({ "originator", originator });
    }
    const std::string user_agent = trim_ascii(req.get_header_value("User-Agent"));
    if (!user_agent.empty()) {
        prepared.headers.push_back({ "User-Agent", user_agent });
    }
    const std::string accept_language = trim_ascii(req.get_header_value("Accept-Language"));
    if (!accept_language.empty()) {
        prepared.headers.push_back({ "Accept-Language", accept_language });
    }
    prepared.body = body;

    const bool allow_private_target = upstream_channel_allows_private_target(config().compact_gateway_base_url);
    try {
        const UpstreamResponse upstream = default_upstream_http_transport(
            prepared, config().proxy_upstream_timeout_seconds * 1000, allow_private_target);
        if (upstream.body.size() > static_cast<size_t>(config().http_max_body_bytes)) {
            return http_response(
                502, "Bad Gateway",
                boost::json::object{
                    { "error", boost::json::object{ { "message", "compact upstream response too large" } } } },
                { { "X-Request-Id", std::string{ request_id } } });
        }

        std::string body_bytes = upstream.body;
        const std::string response_id = upstream_response_id_from_headers(upstream.headers);

        if (upstream.status_code >= 400) {
            Request usage_request = make_proxy_usage_request(selection->auth, selection->forwarded_model,
                                                             "/v1/responses/compact", usage_event_id, 0,
                                                             upstream.status_code, false);
            assign_request_correlation(usage_request, request_id, response_id);
            (void)commit_proxy_usage(usage_request, nullptr);
            return make_upstream_http_response(upstream.status_code, std::move(body_bytes),
                                               merge_correlation_headers(upstream.headers, request_id, response_id));
        }

        Request usage_request = make_proxy_usage_request(selection->auth, selection->forwarded_model,
                                                         "/v1/responses/compact", usage_event_id, 0,
                                                         upstream.status_code, false);
        assign_request_correlation(usage_request, request_id, response_id);
        if (const auto response_tier = parse_json_string_field(body_bytes, "service_tier"); response_tier.has_value()) {
            usage_request.service_tier = *response_tier;
        }
        std::unique_ptr<Request> billing_request;
        if (const auto billing_model = billing_model_for_name(selection->forwarded_model); billing_model.has_value()) {
            billing_request = std::make_unique<Request>(parse_billing_request_from_body(
                GatewayStreamKind::openai_responses, *billing_model, selection->auth.user_id, body_bytes, 1.0,
                selection->route_group_multiplier));
        }
        if (!commit_proxy_usage(usage_request, billing_request.get())) {
            return http_response(
                502, "Bad Gateway",
                boost::json::object{ { "error", boost::json::object{ { "message", "usage commit failed" } } } },
                { { "X-Request-Id", std::string{ request_id } } });
        }
        return make_upstream_http_response(upstream.status_code, std::move(body_bytes),
                                           merge_correlation_headers(upstream.headers, request_id, response_id));
    } catch (const std::exception &) {
        return http_response(
            502, "Bad Gateway",
            boost::json::object{ { "error", boost::json::object{ { "message", "compact gateway failed" } } } },
            { { "X-Request-Id", std::string{ request_id } } });
    }
}

void run_responses_compact_stream(::httplib::Response &res, const ::httplib::Request &req,
                                  const GatewayParsedRequest &parsed, std::string_view request_id,
                                  long long usage_event_id, std::string_view client_ip)
{
    (void)parsed;
    (void)client_ip;
    const RequireProxyAuthResult auth_gate = require_proxy_auth(authenticated_token(req), request_id);
    if (!auth_gate.auth.has_value()) {
        apply_http_response(*auth_gate.error, res);
        return;
    }
    const TokenAuth &auth = *auth_gate.auth;
    if (trim_ascii(config().compact_gateway_base_url).empty() || trim_ascii(config().compact_gateway_key).empty()) {
        apply_http_response(
            http_response(
                502, "Bad Gateway",
                boost::json::object{ { "error", boost::json::object{ { "message", "compact gateway unavailable" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
            res);
        return;
    }
    const auto selection = select_compact_gateway_target(req.body, auth);
    if (!selection.has_value()) {
        apply_http_response(
            http_response(
                400, "Bad Request",
                boost::json::object{ { "error", boost::json::object{ { "message", "compact model unavailable" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
            res);
        return;
    }

    if (const auto quota_error = paygo_balance_gate(auth.user_id, request_id); quota_error.has_value()) {
        apply_http_response(*quota_error, res);
        return;
    }

    const std::string body = compact_request_body_for_gateway(req.body, *selection);
    UpstreamPreparedRequest prepared;
    try {
        prepared.base_url = validate_upstream_base_url(config().compact_gateway_base_url);
        enforce_compact_gateway_guard(prepared.base_url);
    } catch (const std::exception &) {
        apply_http_response(
            http_response(
                502, "Bad Gateway",
                boost::json::object{ { "error", boost::json::object{ { "message", "compact gateway unavailable" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
            res);
        return;
    }
    prepared.method = "POST";
    prepared.url = build_upstream_url(prepared.base_url, "/v1/responses/compact", "");
    const std::string content_type = trim_ascii(req.get_header_value("Content-Type"));
    prepared.headers = {
        { "Content-Type", content_type.empty() ? "application/json" : content_type },
        { "Authorization", "Bearer " + trim_ascii(config().compact_gateway_key) },
        { "X-Request-Id", std::string{ request_id } },
    };
    const std::string session_id = trim_ascii(req.get_header_value("session_id"));
    if (!session_id.empty()) {
        prepared.headers.push_back({ "session_id", session_id });
    }
    const std::string originator = trim_ascii(req.get_header_value("originator"));
    if (!originator.empty()) {
        prepared.headers.push_back({ "originator", originator });
    }
    const std::string user_agent = trim_ascii(req.get_header_value("User-Agent"));
    if (!user_agent.empty()) {
        prepared.headers.push_back({ "User-Agent", user_agent });
    }
    const std::string accept_language = trim_ascii(req.get_header_value("Accept-Language"));
    if (!accept_language.empty()) {
        prepared.headers.push_back({ "Accept-Language", accept_language });
    }
    prepared.body = body;

    UpstreamStreamResponse upstream;
    try {
        const bool allow_private_target = upstream_channel_allows_private_target(config().compact_gateway_base_url);
        upstream = default_upstream_http_stream_transport(prepared, config().proxy_upstream_timeout_seconds * 1000,
                                                          allow_private_target);
    } catch (const std::exception &) {
        apply_http_response(
            http_response(
                502, "Bad Gateway",
                boost::json::object{ { "error", boost::json::object{ { "message", "compact gateway failed" } } } },
                { { "X-Request-Id", std::string{ request_id } } }),
            res);
        return;
    }

    const int status = upstream.status_code;
    const CompactGatewaySelection committed = *selection;
    const std::string response_id = upstream_response_id_from_headers(upstream.headers);
    const auto stream_billing_model = billing_model_for_name(committed.forwarded_model);
    const std::string requested_service_tier = parse_json_string_field(req.body, "service_tier").value_or("");
    std::unique_ptr<Gateway> stream_gateway;
    if (stream_billing_model.has_value()) {
        stream_gateway = make_gateway(GatewayStreamKind::openai_responses, *stream_billing_model, 1.0,
                                      committed.route_group_multiplier);
    }
    apply_upstream_gateway_stream(
        res, status, upstream.headers, std::move(upstream), std::move(stream_gateway), requested_service_tier,
        committed.auth.user_id,
        [committed, request_id = std::string(request_id), response_id, usage_event_id,
         status](const GatewayStreamResult &result) {
            try {
                const GatewayStreamPump &pump = result.pump;
                if (status >= 400 || !pump.completed || pump.upstream_error || pump.idle_timeout ||
                    !result.billing_request.has_value()) {
                    return;
                }
                Request usage_request = make_proxy_usage_request(committed.auth, committed.forwarded_model,
                                                                 "/v1/responses/compact", usage_event_id, 0, status,
                                                                 true);
                assign_request_correlation(usage_request, request_id, response_id);
                usage_request.first_token_latency_ms = pump.first_token_latency_ms;
                std::unique_ptr<Request> billing_request = std::make_unique<Request>(*result.billing_request);
                if (!commit_proxy_usage(usage_request, billing_request.get())) {
                    std::cerr << "stream compact usage commit failed: " << request_id << '\n';
                }
            } catch (const std::exception &err) {
                std::cerr << "stream compact callback failed: " << err.what() << " request_id=" << request_id << '\n';
            }
        });
    set_stream_correlation_headers(res, request_id, response_id);
}

} // namespace revlm
