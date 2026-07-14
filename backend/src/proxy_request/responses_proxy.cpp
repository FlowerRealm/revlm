#include "proxy_request/responses_proxy.hpp"

#include "billing/billing.hpp"
#include "models/models.hpp"
#include "models/quota.hpp"
#include "proxy_request/gateway_resilience.hpp"
#include "proxy_request/http_client.hpp"
#include "proxy_request/routing_data_source.hpp"
#include "proxy_request/token_auth.hpp"
#include "proxy_request/upstream.hpp"
#include "proxy_response/api_stream.hpp"
#include "proxy_response/upstream_http.hpp"
#include "scheduler/scheduler.hpp"
#include "server/tokens.hpp"
#include "request/request.hpp"
#include "store/database.hpp"
#include "util/user_input.hpp"

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
#include <unordered_set>
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
};

struct ProxyUpstreamResponse {
    int status = 502;
    std::string headers;
    std::string body;
    std::string content_type;
    bool sse = false;
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

using GatewayRoutingDataSource = ProxyRoutingDataSource;

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
            if (static_cast<unsigned char>(ch) < 0x20) {
                out += "\\u00";
                constexpr char hex[] = "0123456789abcdef";
                out.push_back(hex[(ch >> 4) & 0xf]);
                out.push_back(hex[ch & 0xf]);
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    return out;
}

std::string_view request_body(std::string_view request)
{
    const size_t body_start = request.find("\r\n\r\n");
    if (body_start == std::string_view::npos) {
        return {};
    }
    return request.substr(body_start + 4);
}

std::string json_error_body(std::string_view message)
{
    return "{\"error\":{\"message\":\"" + json_escape(message) + "\"}}\n";
}

std::optional<std::string> model_from_request_json(std::string_view body)
{
    const std::string needle = "\"model\"";
    size_t pos = body.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos = body.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])) != 0) {
        ++pos;
    }
    if (pos >= body.size() || body[pos] != '"') {
        return std::nullopt;
    }
    ++pos;
    std::string out;
    while (pos < body.size()) {
        char ch = body[pos++];
        if (ch == '"') {
            return trim_ascii(out);
        }
        if (ch == '\\' && pos < body.size()) {
            out.push_back(body[pos++]);
            continue;
        }
        out.push_back(ch);
    }
    return std::nullopt;
}

std::optional<bool> bool_from_request_json(std::string_view body, std::string_view field_name)
{
    const std::string needle = "\"" + std::string(field_name) + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos = body.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])) != 0) {
        ++pos;
    }
    if (body.substr(pos, 4) == "true") {
        return true;
    }
    if (body.substr(pos, 5) == "false") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::string> string_from_request_json(std::string_view body, std::string_view field_name)
{
    const std::string needle = "\"" + std::string(field_name) + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos = body.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])) != 0) {
        ++pos;
    }
    if (pos >= body.size() || body[pos] != '"') {
        return std::nullopt;
    }
    ++pos;
    std::string out;
    while (pos < body.size()) {
        char ch = body[pos++];
        if (ch == '"') {
            return trim_ascii(out);
        }
        if (ch == '\\' && pos < body.size()) {
            out.push_back(body[pos++]);
            continue;
        }
        out.push_back(ch);
    }
    return std::nullopt;
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

std::optional<Model> billing_model_for_name(std::string_view name)
{
    const std::vector<Model> &models = ModelManager::instance().models();
    const auto it = std::ranges::find(models, name, &Model::name);
    if (it == models.end()) {
        return std::nullopt;
    }
    return *it;
}

std::optional<Model> validate_requested_model(const TokenAuth &auth, odb::database &db,
                                              std::string_view requested_model, std::string *resolved_model_out)
{
    std::string response_id = trim_ascii(requested_model);
    if (response_id.empty()) {
        throw std::invalid_argument("model is required");
    }
    std::string resolved_id = response_id;
    if (const auto mapped = resolve_model_mapping(auth, response_id); mapped.second) {
        resolved_id = mapped.first;
    }

    const std::optional<Model> model = billing_model_for_name(resolved_id);
    if (!model.has_value()) {
        return std::nullopt;
    }

    bool allow_openai = false;
    bool allow_anthropic = false;
    {
        ScopedTransaction t(db);
        const auto type_rows =
            sql_query_rows(db, "SELECT DISTINCT c.type FROM channels c "
                               "JOIN channel_group_members cgm ON cgm.channel_id=c.id "
                               "JOIN channel_groups cg ON cg.id=cgm.channel_group_id AND cg.status=1 "
                               "JOIN token_channel_groups tcg ON tcg.channel_group_id=cg.id "
                               "WHERE tcg.token_id=" +
                                   std::to_string(auth.token_id) + " AND c.status=1");
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
                                                     std::string_view resolved_model, std::string_view request_path)
{
    SchedulerConstraints constraints;
    constraints.allowed_groups = auth.groups;
    constraints.requested_model = std::string(resolved_model);
    if (model.owned_by == "anthropic") {
        constraints.required_api = SchedulerApi::anthropic;
    } else {
        constraints.required_api = SchedulerApi::openai;
    }
    if (request_path == "/v1/responses/input_tokens") {
        constraints.sequential_channel_failover = false;
    }
    return constraints;
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
    const std::string needle = "\"" + std::string(field_name) + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos = body.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])) != 0) {
        ++pos;
    }
    if (pos >= body.size() || body[pos] != '"') {
        return std::nullopt;
    }
    ++pos;
    std::string out;
    while (pos < body.size()) {
        const char ch = body[pos++];
        if (ch == '"') {
            return trim_ascii(out);
        }
        if (ch == '\\' && pos < body.size()) {
            out.push_back(body[pos++]);
            continue;
        }
        out.push_back(ch);
    }
    return std::nullopt;
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

bool should_retry_status(int status)
{
    return status == 408 || status == 409 || status == 429 || status >= 500;
}

SchedulerResult scheduler_result_from_status(int status)
{
    SchedulerResult result;
    result.success = status >= 200 && status < 300;
    result.status_code = status;
    result.retriable = should_retry_status(status);
    result.failure_scope = SchedulerFailureScope::channel;
    if (status >= 400) {
        result.error_class = "upstream_status";
    }
    return result;
}

ProxyUpstreamResponse perform_upstream_request(const SchedulerSelection &selection, std::string_view path,
                                               std::string_view method, std::string_view request_body_json,
                                               const Config &config, std::string_view request_id,
                                               std::string_view service_tier)
{
    UpstreamExecutor executor;
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

    const int timeout_ms = config.proxy_upstream_timeout_seconds * 1000;
    const bool allow_private_target = upstream_channel_allows_private_target(selection.base_url);
    const UpstreamExecutionResult executed =
        execute_with_default_transport(executor, selection, downstream, timeout_ms, allow_private_target);

    std::string content_type;
    for (const auto &header : executed.response.headers) {
        if (lowercase_ascii(header.name) == "content-type") {
            content_type = trim_ascii(header.value);
            break;
        }
    }
    return ProxyUpstreamResponse{
        .status = executed.response.status_code,
        .headers = format_upstream_proxy_response_headers(executed.response.status_code, executed.response.headers,
                                                          executed.response.body.size()),
        .body = executed.response.body,
        .content_type = content_type,
        .sse = is_sse_content_type(content_type),
    };
}

UpstreamSession open_upstream_stream_session(const SchedulerSelection &selection, std::string_view path,
                                             std::string_view method, std::string_view request_body_json,
                                             const Config &config, std::string_view request_id,
                                             std::string_view service_tier)
{
    UpstreamExecutor executor;
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

    const int timeout_ms = config.proxy_upstream_timeout_seconds * 1000;
    const bool allow_private_target = upstream_channel_allows_private_target(selection.base_url);
    const UpstreamPreparedRequest prepared = executor.prepare(selection, downstream, false, !allow_private_target);
    UpstreamStreamResponse stream_response =
        execute_upstream_http_stream_request(prepared, timeout_ms, allow_private_target);

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
    session.stream = std::move(stream_response.stream);
    return session;
}

HttpResponse finalize_non_stream_usage(const Config &config, std::string_view request_id, const TokenAuth &auth,
                                       const SchedulerSelection &selection, std::string_view endpoint_path,
                                       std::string_view forwarded_model, std::string_view requested_service_tier,
                                       const ProxyUpstreamResponse &upstream, int latency_ms)
{
    HttpResponse response;
    response.status = upstream.status;
    response.reason = (upstream.status >= 200 && upstream.status < 300) ? "OK" : "Bad Gateway";
    response.body = upstream.body;
    response.content_type = upstream.content_type.empty() ? "application/json; charset=utf-8" : upstream.content_type;
    response.headers.push_back({ "X-Request-Id", std::string{ request_id } });

    if (upstream.status >= 400) {
        return response;
    }

    const long long usage_event_id = parse_long_long(request_id).value_or(0);
    if (usage_event_id <= 0) {
        return http_response(502, "Bad Gateway", "usage commit failed\n", "text/plain; charset=utf-8", request_id);
    }

    auto db = make_database(config.db_dsn);
    const std::optional<Model> billing_model = billing_model_for_name(forwarded_model);
    if (!billing_model.has_value()) {
        return http_response(502, "Bad Gateway", "usage commit failed\n", "text/plain; charset=utf-8", request_id);
    }
    auto billing_request = std::make_unique<Request>(parse_responses_billing_request(
        *billing_model, auth.user_id, upstream.body, requested_service_tier, selection.route_group_multiplier));
    const std::optional<std::string> response_service_tier = json_string_value(upstream.body, "service_tier");

    Quota(*db).charge(*billing_request);
    Request request = *billing_request;
    request.id = usage_event_id;
    request.user_id = auth.user_id;
    request.token_id = auth.token_id;
    if (response_service_tier.has_value()) {
        request.service_tier = normalize_usage_service_tier(std::string_view(*response_service_tier));
    }
    request.endpoint = std::string(endpoint_path);
    request.method = "POST";
    request.status_code = upstream.status;
    request.channel_id = selection.channel_id;
    request.is_stream = false;
    request.statue = true;
    request.latency_ms = std::max(latency_ms, 0);

    if (!request.commit(*db, request_timestamp_now())) {
        return http_response(502, "Bad Gateway", "usage commit failed\n", "text/plain; charset=utf-8", request_id);
    }

    return response;
}

bool commit_stream_usage(const Config &config, std::string_view request_id, const TokenAuth &auth,
                         const SchedulerSelection &selection, std::string_view endpoint_path,
                         std::string_view forwarded_model, Request &billing_request,
                         const std::optional<std::string> &response_service_tier, int status_code, int latency_ms,
                         int first_token_latency_ms)
{
    if (status_code >= 400) {
        return true;
    }
    const long long usage_event_id = parse_long_long(request_id).value_or(0);
    if (usage_event_id <= 0) {
        std::cerr << "stream usage commit failed: invalid usage event id: " << request_id << '\n';
        return false;
    }

    auto db = make_database(config.db_dsn);
    billing_request.channel_multiplier = selection.route_group_multiplier;
    Quota(*db).charge(billing_request);
    Request request = billing_request;
    request.id = usage_event_id;
    request.user_id = auth.user_id;
    request.token_id = auth.token_id;
    if (request.model.name.empty()) {
        request.model.name = std::string(forwarded_model);
    }
    if (response_service_tier.has_value()) {
        request.service_tier = normalize_usage_service_tier(std::string_view(*response_service_tier));
    }
    request.endpoint = std::string(endpoint_path);
    request.method = "POST";
    request.status_code = status_code;
    request.channel_id = selection.channel_id;
    request.is_stream = true;
    request.statue = true;
    request.latency_ms = std::max(latency_ms, 0);
    request.first_token_latency_ms = std::min(std::max(first_token_latency_ms, 0), request.latency_ms);
    if (!request.commit(*db, request_timestamp_now())) {
        std::cerr << "stream usage commit failed: " << request_id << '\n';
        return false;
    }
    return true;
}

void stream_upstream_session_to_httplib(::httplib::Response &res, UpstreamSession session, const Config &config,
                                        std::string_view request_id, const Model &billing_model, long long user_id,
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

    struct Shared {
        UpstreamSession session;
        std::optional<std::string> upstream_response_model;
        std::string requested_service_tier;
        Model billing_model;
        double channel_multiplier = 1.0;
        long long user_id = 0;
        Config config;
        std::function<void(int, Request &, const std::optional<std::string> &, long long, int)> on_complete;
    };
    auto shared = std::make_shared<Shared>();
    shared->session = std::move(session);
    shared->billing_model = billing_model;
    shared->channel_multiplier = route_group_multiplier;
    shared->user_id = user_id;
    shared->upstream_response_model = json_string_value(shared->session.head.body, "model");
    shared->requested_service_tier = std::string{ requested_service_tier };
    shared->config = config;
    shared->on_complete = std::move(on_complete);

    const int idle_timeout_ms = std::max(1000, config.proxy_upstream_timeout_seconds * 1000);
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

bool stream_upstream_session_to_client(UpstreamSession &session, const ClientWriter &write_client, const Config &config,
                                       std::string_view request_id, Request &billing_request,
                                       std::optional<std::string> &upstream_response_model, long long &response_bytes,
                                       int &first_token_latency_ms)
{
    if (!write_client(
            build_synthetic_stream_response_head(session.head.status, session.head.content_type, request_id))) {
        return false;
    }
    auto gateway = make_gateway(GatewayStreamKind::openai_responses, billing_request.model,
                                billing_request.tier_multiplier, billing_request.channel_multiplier);
    const int idle_timeout_ms = std::max(1000, config.proxy_upstream_timeout_seconds * 1000);
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

std::optional<HttpResponse> paygo_balance_gate(odb::database &db, long long user_id, std::string_view request_id)
{
    if (BillingStore(db).has_positive_user_balance(user_id)) {
        return std::nullopt;
    }
    return http_response(402, "Payment Required", "insufficient balance\n", "text/plain; charset=utf-8", request_id);
}

} // namespace

ResponsesProxyResult handle_responses_proxy_request(std::string_view raw_request, std::string_view method,
                                                    std::string_view path, const Config &config, const BuildInfo &,
                                                    std::string_view request_id,
                                                    const ResponsesProxyExecuteOptions &options)
{
    if (method != "POST") {
        return ResponsesProxyResult{
            .response = http_response(405, "Method Not Allowed", "method not allowed\n", "text/plain; charset=utf-8",
                                      request_id),
        };
    }

    TokenAuthResult auth_result = authenticated_token(raw_request, config);
    if (!auth_result.auth.has_value()) {
        return ResponsesProxyResult{
            .response = http_response(auth_result.status, auth_result.status == 401 ? "Unauthorized" : "Bad Gateway",
                                      auth_result.message + "\n", "text/plain; charset=utf-8", request_id),
        };
    }
    const TokenAuth &auth = *auth_result.auth;
    if (auth.groups.empty()) {
        return ResponsesProxyResult{
            .response =
                http_response(400, "Bad Request", "Token 未配置渠道组\n", "text/plain; charset=utf-8", request_id),
        };
    }

    const std::string body = transform_request_body(path, request_body(raw_request));
    const bool stream_requested = bool_from_request_json(body, "stream").value_or(false);
    const std::optional<std::string> model_from_body = model_from_request_json(body);
    if (!model_from_body.has_value()) {
        return ResponsesProxyResult{
            .response = http_response(400, "Bad Request", json_error_body("model is required"),
                                      "application/json; charset=utf-8", request_id),
        };
    }

    const auto request_started_at = Clock::now();
    const auto elapsed_latency_ms = [&request_started_at]() {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - request_started_at).count());
    };

    try {
        auto db = make_database(config.db_dsn);
        std::string resolved_model;
        const auto model = validate_requested_model(auth, *db, *model_from_body, &resolved_model);
        if (!model.has_value()) {
            return ResponsesProxyResult{
                .response = http_response(404, "Not Found", json_error_body("model is not available"),
                                          "application/json; charset=utf-8", request_id),
            };
        }
        if (const auto quota_error = paygo_balance_gate(*db, auth.user_id, request_id); quota_error.has_value()) {
            return ResponsesProxyResult{ .response = *quota_error };
        }

        const std::string requested_service_tier =
            normalize_service_tier_request(string_from_request_json(body, "service_tier"));

        GatewayRoutingDataSource routing(*db);
        Scheduler scheduler(routing);
        const SchedulerConstraints constraints = scheduler_constraints_for_model(*model, auth, resolved_model, path);
        SchedulerConstraints attempt_constraints = constraints;

        ProxyUpstreamResponse upstream;
        SchedulerSelection selection;
        std::exception_ptr last_error;
        const int max_attempts = std::max(1, config.gateway_max_failover_switches + 1);

        ClientWriter write_client = options.write_client;
        if (!write_client && options.client_fd >= 0) {
            write_client = client_writer_from_fd(options.client_fd);
        }
        const bool stream_sink_ready = write_client || options.stream_response != nullptr;

        if (stream_requested && stream_sink_ready) {
            UpstreamSession session;
            bool have_session = false;
            for (int attempt = 0; attempt < max_attempts; ++attempt) {
                try {
                    selection = {};
                    selection = scheduler.select(auth.user_id, route_key_hash_for_request(auth, request_id),
                                                 attempt_constraints);
                    GatewayCredentialSlotGuard credential_slot(config, selection);
                    if (!credential_slot.ok()) {
                        const GatewayFailure failure = credential_slot.failure();
                        SchedulerResult result;
                        result.success = false;
                        result.retriable = false;
                        result.status_code = failure.status_code;
                        result.error_class = failure.error_class;
                        result.failure_scope = failure.failure_scope;
                        scheduler.report(selection, result);
                        return ResponsesProxyResult{
                            .response = http_response(failure.status_code,
                                                      failure.status_code == 429 ? "Too Many Requests" : "Bad Gateway",
                                                      failure.error_message.empty() ? "proxy upstream failed\n" :
                                                                                      failure.error_message + "\n",
                                                      "text/plain; charset=utf-8", request_id),
                        };
                    }
                    session = open_upstream_stream_session(selection, path, method, body, config, request_id,
                                                           requested_service_tier);
                    scheduler.report(selection, scheduler_result_from_status(session.head.status));
                    if (!should_retry_status(session.head.status) || attempt + 1 >= max_attempts) {
                        have_session = true;
                        break;
                    }
                    attempt_constraints.excluded_channel_ids.insert(selection.channel_id);
                } catch (const std::exception &) {
                    last_error = std::current_exception();
                    if (selection.channel_id > 0) {
                        SchedulerResult result;
                        result.success = false;
                        result.retriable = true;
                        result.status_code = 503;
                        result.error_class = "network";
                        result.failure_scope = SchedulerFailureScope::channel;
                        scheduler.report(selection, result);
                        attempt_constraints.excluded_channel_ids.insert(selection.channel_id);
                    }
                    if (attempt + 1 >= max_attempts) {
                        std::rethrow_exception(last_error);
                    }
                    continue;
                }
            }

            if (!have_session) {
                throw std::runtime_error("stream upstream unavailable");
            }
            if (!is_sse_content_type(session.head.content_type)) {
                upstream = ProxyUpstreamResponse{
                    .status = session.head.status,
                    .headers = session.head.raw_headers,
                    .body = session.head.body + read_remaining_stream(session.stream),
                    .content_type = session.head.content_type,
                    .sse = false,
                };
            } else {
                const std::optional<Model> billing_model = billing_model_for_name(resolved_model);
                if (!billing_model.has_value()) {
                    throw std::runtime_error("billing model unavailable");
                }
                const int stream_status = session.head.status;
                const std::optional<std::string> head_response_model = json_string_value(session.head.body, "model");
                const std::optional<std::string> head_response_service_tier =
                    json_string_value(session.head.body, "service_tier");
                if (options.stream_response != nullptr) {
                    stream_upstream_session_to_httplib(
                        *options.stream_response, std::move(session), config, request_id, *billing_model, auth.user_id,
                        requested_service_tier, selection.route_group_multiplier,
                        [&](int, Request &stream_request, const std::optional<std::string> &, long long,
                            int first_token_latency_ms) {
                            if (stream_status >= 400) {
                                return;
                            }
                            if (!commit_stream_usage(config, request_id, auth, selection, path, resolved_model,
                                                     stream_request, head_response_service_tier, stream_status,
                                                     elapsed_latency_ms(), first_token_latency_ms)) {
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
                if (!stream_upstream_session_to_client(session, write_client, config, request_id, *billing_request,
                                                       upstream_response_model, response_bytes,
                                                       first_token_latency_ms)) {
                    throw std::runtime_error("stream pump failed");
                }
                if (session.head.status < 400) {
                    if (!commit_stream_usage(config, request_id, auth, selection, path, resolved_model,
                                             *billing_request, head_response_service_tier, session.head.status,
                                             elapsed_latency_ms(), first_token_latency_ms)) {
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

        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            try {
                selection = {};
                selection =
                    scheduler.select(auth.user_id, route_key_hash_for_request(auth, request_id), attempt_constraints);
                GatewayCredentialSlotGuard credential_slot(config, selection);
                if (!credential_slot.ok()) {
                    const GatewayFailure failure = credential_slot.failure();
                    SchedulerResult result;
                    result.success = false;
                    result.retriable = false;
                    result.status_code = failure.status_code;
                    result.error_class = failure.error_class;
                    result.failure_scope = failure.failure_scope;
                    scheduler.report(selection, result);
                    return ResponsesProxyResult{
                        .response = http_response(
                            failure.status_code, failure.status_code == 429 ? "Too Many Requests" : "Bad Gateway",
                            failure.error_message.empty() ? "proxy upstream failed\n" : failure.error_message + "\n",
                            "text/plain; charset=utf-8", request_id),
                    };
                }
                upstream =
                    perform_upstream_request(selection, path, method, body, config, request_id, requested_service_tier);
                scheduler.report(selection, scheduler_result_from_status(upstream.status));
                if (!should_retry_status(upstream.status) || attempt + 1 >= max_attempts) {
                    break;
                }
                attempt_constraints.excluded_channel_ids.insert(selection.channel_id);
            } catch (const std::exception &) {
                last_error = std::current_exception();
                if (selection.channel_id > 0) {
                    SchedulerResult result;
                    result.success = false;
                    result.retriable = true;
                    result.status_code = 503;
                    result.error_class = "network";
                    result.failure_scope = SchedulerFailureScope::channel;
                    scheduler.report(selection, result);
                    attempt_constraints.excluded_channel_ids.insert(selection.channel_id);
                }
                if (attempt + 1 >= max_attempts) {
                    std::rethrow_exception(last_error);
                }
            }
        }

        if (path == "/v1/responses/input_tokens") {
            HttpResponse response;
            response.status = upstream.status;
            response.reason = (upstream.status >= 200 && upstream.status < 300) ? "OK" : "Bad Gateway";
            response.body = upstream.body;
            response.content_type = upstream.content_type.empty() ? "application/json; charset=utf-8" :
                                                                    upstream.content_type;
            response.headers.push_back({ "X-Request-Id", std::string{ request_id } });
            return ResponsesProxyResult{
                .response = std::move(response),
            };
        }

        return ResponsesProxyResult{
            .response = finalize_non_stream_usage(config, request_id, auth, selection, path, resolved_model,
                                                  requested_service_tier, upstream, elapsed_latency_ms()),
        };
    } catch (const std::invalid_argument &err) {
        return ResponsesProxyResult{
            .response = http_response(400, "Bad Request", json_error_body(err.what()),
                                      "application/json; charset=utf-8", request_id),
        };
    } catch (const std::exception &err) {
        return ResponsesProxyResult{
            .response = http_response(502, "Bad Gateway", json_error_body(err.what()),
                                      "application/json; charset=utf-8", request_id),
        };
    }
}

} // namespace revlm
