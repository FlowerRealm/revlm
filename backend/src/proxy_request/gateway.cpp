#include "proxy_request/gateway.hpp"

#include "auth/security.hpp"
#include "auth/users.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "models/models.hpp"
#include "proxy_request/gateway_resilience.hpp"
#include "proxy_request/routing_data_source.hpp"
#include "proxy_request/token_auth.hpp"
#include "proxy_request/upstream.hpp"
#include "proxy_response/api_stream.hpp"
#include "proxy_response/upstream_http.hpp"
#include "models/quota.hpp"
#include "request/request.hpp"
#include "scheduler/scheduler.hpp"
#include "server/tokens.hpp"
#include "store/database.hpp"
#include "util/json_util.hpp"
#include "util/user_input.hpp"

#include <boost/json.hpp>
#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
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

struct JsonField {
    std::string name;
    std::string value;
};

struct JsonObjectField {
    std::string name;
    std::string raw;
    bool is_string = false;
};

constexpr std::string_view gateway_type_openai = "openai_compatible";
constexpr int gateway_channel_type_anthropic = 4;

struct MessagesProxySelection {
    TokenAuth auth;
    Channel channel;
    std::string requested_model;
    std::string forwarded_model;
    std::string route_group;
    double route_group_multiplier = 1.0;
};

struct CompactGatewaySelection {
    TokenAuth auth;
    std::string requested_model;
    std::string forwarded_model;
    std::string route_group;
    double route_group_multiplier = 1.0;
};

struct SchedulerChatSelection {
    TokenAuth auth;
    SchedulerSelection selection;
    std::string requested_model;
    std::string forwarded_model;
};

struct GatewayAttemptResult {
    SchedulerChatSelection selection;
    int status_code = 502;
    std::vector<UpstreamHeader> response_headers;
    std::string body_bytes;
};

struct GatewayAttemptExecution {
    std::optional<GatewayAttemptResult> result;
    std::optional<GatewayAttemptTransportError> transport_error;
};

struct GatewayStreamAttemptResult {
    SchedulerChatSelection selection;
    UpstreamStreamResponse upstream;
};

struct GatewayStreamAttemptExecution {
    std::optional<GatewayStreamAttemptResult> result;
    std::optional<GatewayAttemptTransportError> transport_error;
};

struct GatewayFailureRecord {
    GatewayFailure failure;
    std::optional<SchedulerChatSelection> selection;
    std::vector<UpstreamHeader> response_headers;
    std::string body_bytes;
};

struct ProxyGatewayContext {
    std::unique_ptr<odb::database> db;
    ProxyRoutingDataSource data_source;
    Scheduler scheduler;

    explicit ProxyGatewayContext(const Config &config)
        : db(make_database(config.db_dsn))
        , data_source(*db)
        , scheduler(data_source)
    {
    }
};

bool parse_json_object_strings(std::string_view json, std::vector<JsonField> &fields)
{
    std::vector<std::pair<std::string, std::string>> parsed;
    if (!parse_json_object_string_fields(json, parsed)) {
        return false;
    }
    fields.clear();
    fields.reserve(parsed.size());
    for (auto &[name, value] : parsed) {
        fields.push_back(JsonField{ std::move(name), std::move(value) });
    }
    return true;
}

std::string json_field(const std::vector<JsonField> &fields, std::string_view name)
{
    for (const JsonField &field : fields) {
        if (field.name == name) {
            return field.value;
        }
    }
    return {};
}

std::optional<std::string> json_string_field_from_body(std::string_view json, std::string_view field_name)
{
    std::vector<JsonField> fields;
    if (!parse_json_object_strings(json, fields)) {
        return std::nullopt;
    }
    const std::string value = json_field(fields, field_name);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
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

std::optional<HttpResponse> paygo_balance_gate(odb::database &db, long long user_id, std::string_view request_id)
{
    if (UserStore(db).has_positive_user_balance(user_id)) {
        return std::nullopt;
    }
    return http_response(402, "Payment Required", "insufficient balance\n", "text/plain; charset=utf-8", request_id);
}

std::string replace_json_string_field(std::string_view json, std::string_view field_name, std::string_view replacement)
{
    boost::system::error_code ec;
    boost::json::value value = boost::json::parse(json, ec);
    if (ec || !value.is_object()) {
        return std::string{ json };
    }
    boost::json::object &object = value.as_object();
    if (auto *field = object.if_contains(field_name); field != nullptr && field->is_string()) {
        *field = std::string{ replacement };
    }
    return boost::json::serialize(value);
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
bool is_hop_by_hop_header(std::string_view name)
{
    const std::string lower = lowercase_ascii(std::string{ name });
    return lower == "host" || lower == "connection" || lower == "keep-alive" || lower == "proxy-authenticate" ||
           lower == "proxy-authorization" || lower == "te" || lower == "trailer" || lower == "transfer-encoding" ||
           lower == "upgrade" || lower == "x-forwarded-for" || lower == "x-forwarded-host" ||
           lower == "x-forwarded-proto" || lower == "x-revlm-remote-ip" || lower == "x-revlm-client-ip";
}

std::vector<UpstreamHeader> gateway_upstream_forward_headers(const ::httplib::Request &req, std::string_view request_id,
                                                             std::string_view client_ip,
                                                             std::function<bool(std::string_view)> drop_header = {})
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

UpstreamRequest build_gateway_upstream_request(const ::httplib::Request &req, std::string_view path,
                                               std::string_view request_id, std::string_view client_ip,
                                               std::string_view body,
                                               std::function<bool(std::string_view)> drop_header = {})
{
    UpstreamRequest downstream;
    downstream.method = "POST";
    downstream.path = std::string{ path };
    downstream.body = std::string{ body };
    downstream.headers = gateway_upstream_forward_headers(req, request_id, client_ip, std::move(drop_header));
    return downstream;
}

GatewayAttemptExecution execute_chat_gateway_attempt(const SchedulerChatSelection &selection,
                                                     const ::httplib::Request &req, const Config &config,
                                                     std::string_view request_id)
{
    const std::string body = selection.requested_model == selection.forwarded_model ?
                                 req.body :
                                 replace_json_string_field(req.body, "model", selection.forwarded_model);
    const UpstreamRequest downstream = build_gateway_upstream_request(
        req, "/v1/chat/completions", request_id, req.get_header_value("X-Revlm-Client-Ip"), body,
        [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });

    UpstreamExecutor executor;
    try {
        const int timeout_ms = config.proxy_upstream_timeout_seconds * 1000;
        const bool allow_private_target = upstream_channel_allows_private_target(selection.selection.base_url);
        const UpstreamExecutionResult executed =
            execute_with_default_transport(executor, selection.selection, downstream, timeout_ms, allow_private_target);
        GatewayAttemptResult result{
            .selection = selection,
            .status_code = executed.response.status_code,
            .response_headers = executed.response.headers,
            .body_bytes = executed.response.body,
        };
        return GatewayAttemptExecution{
            .result = std::move(result),
            .transport_error = std::nullopt,
        };
    } catch (const std::invalid_argument &) {
        return GatewayAttemptExecution{
            .result = std::nullopt,
            .transport_error =
                GatewayAttemptTransportError{
                    .stage = "parse",
                    .message = "upstream URL is invalid",
                },
        };
    } catch (const std::exception &) {
        return GatewayAttemptExecution{
            .result = std::nullopt,
            .transport_error =
                GatewayAttemptTransportError{
                    .stage = "connect",
                    .message = "upstream connect failed",
                },
        };
    }
}

GatewayStreamAttemptExecution execute_chat_gateway_stream_attempt(const SchedulerChatSelection &selection,
                                                                  const ::httplib::Request &req, const Config &config,
                                                                  std::string_view request_id)
{
    const std::string body = selection.requested_model == selection.forwarded_model ?
                                 req.body :
                                 replace_json_string_field(req.body, "model", selection.forwarded_model);
    const UpstreamRequest downstream = build_gateway_upstream_request(
        req, "/v1/chat/completions", request_id, req.get_header_value("X-Revlm-Client-Ip"), body,
        [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });

    UpstreamExecutor executor;
    try {
        const int timeout_ms = config.proxy_upstream_timeout_seconds * 1000;
        const bool allow_private_target = upstream_channel_allows_private_target(selection.selection.base_url);
        const UpstreamPreparedRequest prepared =
            executor.prepare(selection.selection, downstream, false, !allow_private_target);
        UpstreamStreamResponse upstream =
            default_upstream_http_stream_transport(prepared, timeout_ms, allow_private_target);
        return GatewayStreamAttemptExecution{
            .result =
                GatewayStreamAttemptResult{
                    .selection = selection,
                    .upstream = std::move(upstream),
                },
            .transport_error = std::nullopt,
        };
    } catch (const std::invalid_argument &) {
        return GatewayStreamAttemptExecution{
            .result = std::nullopt,
            .transport_error =
                GatewayAttemptTransportError{
                    .stage = "parse",
                    .message = "upstream URL is invalid",
                },
        };
    } catch (const std::exception &) {
        return GatewayStreamAttemptExecution{
            .result = std::nullopt,
            .transport_error =
                GatewayAttemptTransportError{
                    .stage = "connect",
                    .message = "upstream connect failed",
                },
        };
    }
}

std::optional<SchedulerChatSelection> select_chat_proxy_target_with_scheduler(
    std::string_view body, ProxyGatewayContext &gateway, const Config &config, const TokenAuth &auth,
    const std::unordered_set<long long> &excluded_channel_ids, long long start_channel_id)
{
    const auto requested_model = json_string_field_from_body(body, "model");
    if (!requested_model.has_value()) {
        return std::nullopt;
    }

    std::string forwarded_model = *requested_model;
    const std::vector<Model> &models = ModelManager::instance().models();
    if (std::ranges::find(models, forwarded_model, &Model::name) == models.end()) {
        return std::nullopt;
    }
    gateway.scheduler.set_cooldown_base(std::chrono::milliseconds(config.gateway_retry_base_delay_ms));

    SchedulerConstraints constraints;
    constraints.required_api = SchedulerApi::openai;
    constraints.allowed_groups = auth.groups;
    constraints.allowed_group_order = auth.groups;
    constraints.requested_model = forwarded_model;
    constraints.soft_excluded_channel_ids = excluded_channel_ids;
    constraints.sequential_channel_failover = true;
    constraints.start_channel_id = start_channel_id;
    constraints.start_channel_exclusive = start_channel_id > 0;

    const std::string route_key =
        std::to_string(auth.user_id) + ":" + std::to_string(auth.token_id) + ":" + forwarded_model;
    SchedulerSelection scheduled;
    try {
        scheduled = gateway.scheduler.select(auth.user_id, gateway.scheduler.route_key_hash(route_key), constraints);
    } catch (const std::runtime_error &) {
        return std::nullopt;
    }
    if (scheduled.channel_type != gateway_type_openai) {
        return std::nullopt;
    }

    if (trim_ascii(scheduled.api_key).empty()) {
        return std::nullopt;
    }

    return SchedulerChatSelection{
        .auth = auth,
        .selection = std::move(scheduled),
        .requested_model = *requested_model,
        .forwarded_model = forwarded_model,
    };
}

bool token_route_group_allows_channel_groups(std::string_view channel_groups_csv, std::string_view group_name)
{
    size_t start = 0;
    while (start <= channel_groups_csv.size()) {
        const size_t next = channel_groups_csv.find(',', start);
        const size_t end = next == std::string_view::npos ? channel_groups_csv.size() : next;
        if (trim_ascii(channel_groups_csv.substr(start, end - start)) == group_name) {
            return true;
        }
        if (next == std::string_view::npos) {
            break;
        }
        start = next + 1;
    }
    return false;
}

std::optional<std::string> resolve_token_route_group_name(const TokenAuth &auth, std::string_view channel_groups_csv)
{
    for (const std::string &allowed : auth.groups) {
        if (channel_groups_csv.empty() || token_route_group_allows_channel_groups(channel_groups_csv, allowed)) {
            return allowed;
        }
    }
    return std::nullopt;
}

std::pair<double, std::optional<std::string>>
group_multiplier_for_route_group(odb::database &db, const std::optional<std::string> &route_group)
{
    if (!route_group.has_value() || route_group->empty()) {
        return { 1.0, std::nullopt };
    }
    for (const ChannelGroup &group : ChannelGroupStore(db).list_channel_groups()) {
        if (group.name == *route_group && group.status == 1) {
            return { group.price_multiplier, route_group };
        }
    }
    return { 1.0, std::nullopt };
}

void populate_paygo_route_group(odb::database &db, const TokenAuth &auth, std::string_view channel_groups_csv,
                                std::string &route_group, double &route_group_multiplier)
{
    const auto resolved =
        group_multiplier_for_route_group(db, resolve_token_route_group_name(auth, channel_groups_csv));
    route_group_multiplier = resolved.first;
    route_group = resolved.second.value_or("");
}

std::optional<MessagesProxySelection> select_messages_proxy_target(std::string_view body, const Config &config,
                                                                   const TokenAuth &auth)
{
    const auto requested_model = json_string_field_from_body(body, "model");
    if (!requested_model.has_value()) {
        return std::nullopt;
    }

    std::string forwarded_model = *requested_model;

    auto db = make_database(config.db_dsn);
    const std::vector<Model> &models = ModelManager::instance().models();
    if (std::ranges::find(models, forwarded_model, &Model::name) == models.end()) {
        return std::nullopt;
    }

    ProxyRoutingDataSource data_source(*db);
    Scheduler scheduler(data_source);
    scheduler.rebuild_routing_snapshot();
    const SchedulerRoutingSnapshot *snapshot = scheduler.routing_snapshot();
    if (snapshot == nullptr) {
        return std::nullopt;
    }
    for (const Channel &channel : snapshot->channels) {
        if (!snapshot->channel_supports_model(channel.id, forwarded_model)) {
            continue;
        }
        if (channel.type != gateway_channel_type_anthropic || !channel.status || trim_ascii(channel.api_key).empty()) {
            continue;
        }
        std::string channel_groups_csv;
        if (const auto names_it = snapshot->group_names_by_channel.find(channel.id);
            names_it != snapshot->group_names_by_channel.end()) {
            for (size_t i = 0; i < names_it->second.size(); ++i) {
                if (i > 0) {
                    channel_groups_csv.push_back(',');
                }
                channel_groups_csv += names_it->second[i];
            }
        }
        MessagesProxySelection selection;
        selection.auth = auth;
        selection.channel = channel;
        selection.requested_model = *requested_model;
        selection.forwarded_model = forwarded_model;
        populate_paygo_route_group(*db, auth, channel_groups_csv, selection.route_group,
                                   selection.route_group_multiplier);
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

std::optional<CompactGatewaySelection> select_compact_gateway_target(std::string_view body, const Config &config,
                                                                     const TokenAuth &auth)
{
    const auto requested_model = json_string_field_from_body(body, "model");
    if (!requested_model.has_value()) {
        return std::nullopt;
    }

    std::string mapped_model = *requested_model;

    auto db = make_database(config.db_dsn);
    const std::vector<Model> &models = ModelManager::instance().models();
    if (std::ranges::find(models, mapped_model, &Model::name) == models.end()) {
        return std::nullopt;
    }

    CompactGatewaySelection selection;
    selection.auth = auth;
    selection.requested_model = *requested_model;
    selection.forwarded_model = mapped_model;
    populate_paygo_route_group(*db, auth, "", selection.route_group, selection.route_group_multiplier);
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

void apply_billing_fields(Request &request, const Request &billing_request)
{
    request.model = billing_request.model;
    request.input_tokens = billing_request.input_tokens;
    request.output_tokens = billing_request.output_tokens;
    request.cache_read_tokens = billing_request.cache_read_tokens;
    request.cache_creation_1h_tokens = billing_request.cache_creation_1h_tokens;
    request.cache_creation_5m_tokens = billing_request.cache_creation_5m_tokens;
    request.tier_multiplier = billing_request.tier_multiplier;
    request.channel_multiplier = billing_request.channel_multiplier;
    if (!billing_request.service_tier.null()) {
        request.service_tier = *billing_request.service_tier;
    }
}

Request make_chat_usage_request(const SchedulerChatSelection &selection, long long usage_event_id, int status_code,
                                bool is_stream)
{
    Request request;
    request.id = usage_event_id;
    request.user_id = selection.auth.user_id;
    request.token_id = selection.auth.token_id;
    if (const auto model = billing_model_for_name(selection.forwarded_model); model.has_value()) {
        request.model = *model;
    } else {
        request.model.name = selection.forwarded_model;
    }
    request.endpoint = "/v1/chat/completions";
    request.method = "POST";
    request.status_code = status_code;
    request.channel_id = selection.selection.channel_id;
    request.is_stream = is_stream;
    request.statue = true;
    return request;
}

Request make_messages_usage_request(const MessagesProxySelection &selection, long long usage_event_id, int status_code,
                                    bool is_stream)
{
    Request request;
    request.id = usage_event_id;
    request.user_id = selection.auth.user_id;
    request.token_id = selection.auth.token_id;
    if (const auto model = billing_model_for_name(selection.forwarded_model); model.has_value()) {
        request.model = *model;
    } else {
        request.model.name = selection.forwarded_model;
    }
    request.endpoint = "/v1/messages";
    request.method = "POST";
    request.status_code = status_code;
    request.channel_id = selection.channel.id;
    request.is_stream = is_stream;
    request.statue = true;
    return request;
}

Request make_compact_usage_request(const CompactGatewaySelection &selection, long long usage_event_id, int status_code,
                                   bool is_stream)
{
    Request request;
    request.id = usage_event_id;
    request.user_id = selection.auth.user_id;
    request.token_id = selection.auth.token_id;
    if (const auto model = billing_model_for_name(selection.forwarded_model); model.has_value()) {
        request.model = *model;
    } else {
        request.model.name = selection.forwarded_model;
    }
    request.endpoint = "/v1/responses/compact";
    request.method = "POST";
    request.status_code = status_code;
    request.is_stream = is_stream;
    request.statue = true;
    return request;
}

bool commit_gateway_usage_request(odb::database &db, Request *billing_request, Request &request)
{
    if (request.id <= 0) {
        return false;
    }
    if (billing_request != nullptr) {
        Quota(db).charge(*billing_request);
        apply_billing_fields(request, *billing_request);
    }
    return request.commit(db, request_timestamp_now());
}

bool commit_chat_usage(const Config &config, Request request, Request *billing_request)
{
    auto db = make_database(config.db_dsn);
    return commit_gateway_usage_request(*db, billing_request, request);
}

bool commit_messages_usage(const Config &config, Request request, Request *billing_request)
{
    auto db = make_database(config.db_dsn);
    return commit_gateway_usage_request(*db, billing_request, request);
}

bool commit_compact_usage(const Config &config, Request request, Request *billing_request)
{
    auto db = make_database(config.db_dsn);
    return commit_gateway_usage_request(*db, billing_request, request);
}

} // namespace

HttpResponse run_chat_completions_gateway(const ::httplib::Request &req, const Config &config,
                                          std::string_view request_id)
{
    TokenAuthResult auth_result = authenticated_token(req, config);
    if (!auth_result.auth.has_value()) {
        return http_response(auth_result.status, auth_result.status == 401 ? "Unauthorized" : "Bad Gateway",
                             auth_result.message + "\n", "text/plain; charset=utf-8", request_id);
    }
    const TokenAuth &auth = *auth_result.auth;
    if (auth.groups.empty()) {
        return http_response(400, "Bad Request", "Token 未配置渠道组\n", "text/plain; charset=utf-8", request_id);
    }

    ProxyGatewayContext gateway(config);
    GatewayRetryBudget budget({ .max_attempts = config.gateway_max_retry_attempts,
                                .max_switches = config.gateway_max_failover_switches,
                                .max_elapsed_ms = config.gateway_max_retry_elapsed_ms });
    std::unordered_set<long long> excluded_channels;
    std::vector<GatewayFailureRecord> failures;
    std::optional<SchedulerChatSelection> selection =
        select_chat_proxy_target_with_scheduler(req.body, gateway, config, auth, excluded_channels, 0);
    if (!selection.has_value()) {
        return http_response(400, "Bad Request", "chat completions model unavailable on openai-compatible channels\n",
                             "text/plain; charset=utf-8", request_id);
    }
    if (revlm::parse_json_bool_field(req.body, "stream").value_or(false)) {
        return http_response(400, "Bad Request", "streaming requires live socket path\n", "text/plain; charset=utf-8",
                             request_id);
    }
    if (const auto quota_error = paygo_balance_gate(*gateway.db, auth.user_id, request_id); quota_error.has_value()) {
        return *quota_error;
    }

    const long long usage_event_id = parse_long_long(request_id).value_or(0);
    while (selection.has_value() && budget.can_attempt(!excluded_channels.empty())) {
        const bool switching = !excluded_channels.empty();
        budget.note_attempt(switching);
        const auto execution = execute_chat_gateway_attempt(*selection, req, config, request_id);
        if (!execution.result.has_value()) {
            const auto &transport = *execution.transport_error;
            GatewayFailure failure = classify_gateway_transport_failure(transport.stage, transport.message);
            failures.push_back(GatewayFailureRecord{
                .failure = failure,
                .selection = selection,
                .response_headers = {},
                .body_bytes = {},
            });
            gateway.scheduler.report(selection->selection, gateway_failure_to_scheduler_result(failure));
            excluded_channels.insert(selection->selection.channel_id);
        } else {
            const GatewayAttemptResult &attempt = *execution.result;
            if (attempt.status_code < 400) {
                SchedulerResult ok;
                ok.success = true;
                gateway.scheduler.report(selection->selection, ok);
                Request usage_request =
                    make_chat_usage_request(attempt.selection, usage_event_id, attempt.status_code, false);
                if (const auto response_tier = json_string_field_from_body(attempt.body_bytes, "service_tier");
                    response_tier.has_value()) {
                    usage_request.service_tier = *response_tier;
                }
                std::unique_ptr<Request> billing_request;
                if (const auto billing_model = billing_model_for_name(attempt.selection.forwarded_model);
                    billing_model.has_value()) {
                    billing_request = std::make_unique<Request>(parse_billing_request_from_body(
                        GatewayStreamKind::openai_chat, *billing_model, attempt.selection.auth.user_id,
                        attempt.body_bytes, 1.0, attempt.selection.selection.route_group_multiplier));
                }
                if (!commit_chat_usage(config, usage_request, billing_request.get())) {
                    return http_response(502, "Bad Gateway", "usage commit failed\n", "text/plain; charset=utf-8",
                                         request_id);
                }
                return make_upstream_http_response(attempt.status_code, attempt.response_headers, attempt.body_bytes);
            }

            GatewayFailure failure = classify_gateway_status_failure(attempt.status_code);
            if (!failure.retriable) {
                gateway.scheduler.report(selection->selection, gateway_failure_to_scheduler_result(failure));
                return make_upstream_http_response(attempt.status_code, attempt.response_headers, attempt.body_bytes);
            }
            failures.push_back(GatewayFailureRecord{
                .failure = failure,
                .selection = selection,
                .response_headers = attempt.response_headers,
                .body_bytes = attempt.body_bytes,
            });
            gateway.scheduler.report(selection->selection, gateway_failure_to_scheduler_result(failure));
            excluded_channels.insert(selection->selection.channel_id);
        }

        selection = select_chat_proxy_target_with_scheduler(req.body, gateway, config, auth, excluded_channels,
                                                            selection->selection.channel_id);
    }

    if (failures.empty()) {
        return http_response(502, "Bad Gateway", "proxy upstream unavailable\n", "text/plain; charset=utf-8",
                             request_id);
    }

    std::vector<GatewayFailure> raw_failures;
    raw_failures.reserve(failures.size());
    for (const auto &item : failures) {
        raw_failures.push_back(item.failure);
    }
    const GatewayFailureRecord &best = failures[best_gateway_failure_index(raw_failures)];
    if (best.selection.has_value()) {
        Request usage_request =
            make_chat_usage_request(*best.selection, usage_event_id, best.failure.status_code, false);
        if (!best.failure.error_class.empty()) {
            usage_request.error_class = best.failure.error_class;
        }
        if (!best.failure.error_message.empty()) {
            usage_request.error_message = best.failure.error_message;
        }
        (void)commit_chat_usage(config, usage_request, nullptr);
    }
    if (best.failure.preserve_upstream_response && !best.response_headers.empty()) {
        return make_upstream_http_response(best.failure.status_code, best.response_headers, best.body_bytes);
    }
    const int failure_status = best.failure.status_code >= 400 ? best.failure.status_code : 502;
    const char *failure_reason = failure_status == 429 ? "Too Many Requests" : "Bad Gateway";
    const std::string failure_body = failure_status == 429 && !best.failure.error_message.empty() ?
                                         best.failure.error_message + "\n" :
                                         "proxy upstream failed\n";
    return http_response(failure_status, failure_reason, failure_body, "text/plain; charset=utf-8", request_id);
}

HttpResponse run_messages_gateway(const ::httplib::Request &req, const Config &config, std::string_view request_id)
{
    TokenAuthResult auth_result = authenticated_token(req, config);
    if (!auth_result.auth.has_value()) {
        return http_response(auth_result.status, auth_result.status == 401 ? "Unauthorized" : "Bad Gateway",
                             auth_result.message + "\n", "text/plain; charset=utf-8", request_id);
    }
    const TokenAuth &auth = *auth_result.auth;
    if (auth.groups.empty()) {
        return http_response(400, "Bad Request", "Token 未配置渠道组\n", "text/plain; charset=utf-8", request_id);
    }

    const auto selection = select_messages_proxy_target(req.body, config, auth);
    if (!selection.has_value()) {
        return http_response(400, "Bad Request", "messages model unavailable on anthropic channels\n",
                             "text/plain; charset=utf-8", request_id);
    }

    auto balance_db = make_database(config.db_dsn);
    if (const auto quota_error = paygo_balance_gate(*balance_db, auth.user_id, request_id); quota_error.has_value()) {
        return *quota_error;
    }

    const std::string body = messages_request_body_for_upstream(req.body, *selection);
    if (revlm::parse_json_bool_field(body, "stream").value_or(false)) {
        return http_response(400, "Bad Request", "streaming requires live socket path\n", "text/plain; charset=utf-8",
                             request_id);
    }

    SchedulerSelection scheduled{};
    scheduled.channel_id = selection->channel.id;
    scheduled.channel_type = "anthropic";
    scheduled.base_url = selection->channel.base_url;
    scheduled.api_key = selection->channel.api_key;

    auto db = make_database(config.db_dsn);
    const UpstreamRequest downstream = build_gateway_upstream_request(
        req, "/v1/messages", request_id, req.get_header_value("X-Revlm-Client-Ip"), body,
        [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });

    UpstreamExecutor executor;
    int status = 502;
    std::vector<UpstreamHeader> response_headers;
    std::string body_bytes;
    try {
        const int timeout_ms = config.proxy_upstream_timeout_seconds * 1000;
        const bool allow_private_target = upstream_channel_allows_private_target(scheduled.base_url);
        const UpstreamExecutionResult executed =
            execute_with_default_transport(executor, scheduled, downstream, timeout_ms, allow_private_target);
        status = executed.response.status_code;
        body_bytes = executed.response.body;
        response_headers = executed.response.headers;
    } catch (const std::invalid_argument &) {
        return http_response(502, "Bad Gateway", "proxy upstream unavailable\n", "text/plain; charset=utf-8",
                             request_id);
    } catch (const std::exception &) {
        return http_response(502, "Bad Gateway", "proxy upstream failed\n", "text/plain; charset=utf-8", request_id);
    }

    if (status >= 400) {
        return make_upstream_http_response(status, response_headers, std::move(body_bytes));
    }

    const long long usage_event_id = parse_long_long(request_id).value_or(0);
    Request usage_request = make_messages_usage_request(*selection, usage_event_id, status, false);
    if (const auto response_tier = json_string_field_from_body(body_bytes, "service_tier"); response_tier.has_value()) {
        usage_request.service_tier = *response_tier;
    }
    std::unique_ptr<Request> billing_request;
    if (const auto billing_model = billing_model_for_name(selection->forwarded_model); billing_model.has_value()) {
        billing_request = std::make_unique<Request>(parse_billing_request_from_body(
            GatewayStreamKind::anthropics_messages, *billing_model, selection->auth.user_id, body_bytes, 1.0,
            selection->route_group_multiplier));
    }
    if (!commit_messages_usage(config, usage_request, billing_request.get())) {
        return http_response(502, "Bad Gateway", "usage commit failed\n", "text/plain; charset=utf-8", request_id);
    }

    return make_upstream_http_response(status, response_headers, std::move(body_bytes));
}

HttpResponse run_responses_compact_gateway(const ::httplib::Request &req, const Config &config,
                                           std::string_view request_id)
{
    TokenAuthResult auth_result = authenticated_token(req, config);
    if (!auth_result.auth.has_value()) {
        return http_response(auth_result.status, auth_result.status == 401 ? "Unauthorized" : "Bad Gateway",
                             auth_result.message + "\n", "text/plain; charset=utf-8", request_id);
    }
    const TokenAuth &auth = *auth_result.auth;
    if (auth.groups.empty()) {
        return http_response(400, "Bad Request", "Token 未配置渠道组\n", "text/plain; charset=utf-8", request_id);
    }
    if (trim_ascii(config.compact_gateway_base_url).empty() || trim_ascii(config.compact_gateway_key).empty()) {
        return http_response(502, "Bad Gateway", "compact gateway unavailable\n", "text/plain; charset=utf-8",
                             request_id);
    }
    const auto selection = select_compact_gateway_target(req.body, config, auth);
    if (!selection.has_value()) {
        return http_response(400, "Bad Request", "compact model unavailable\n", "text/plain; charset=utf-8",
                             request_id);
    }

    if (revlm::parse_json_bool_field(req.body, "stream").value_or(false)) {
        return http_response(400, "Bad Request", "streaming requires live socket path\n", "text/plain; charset=utf-8",
                             request_id);
    }

    auto balance_db = make_database(config.db_dsn);
    if (const auto quota_error = paygo_balance_gate(*balance_db, auth.user_id, request_id); quota_error.has_value()) {
        return *quota_error;
    }

    const std::string body = compact_request_body_for_gateway(req.body, *selection);
    UpstreamPreparedRequest prepared;
    prepared.base_url = validate_upstream_base_url(config.compact_gateway_base_url);
    enforce_compact_gateway_guard(prepared.base_url);
    prepared.method = "POST";
    prepared.url = build_upstream_url(prepared.base_url, "/v1/responses/compact", "");
    const std::string content_type = trim_ascii(req.get_header_value("Content-Type"));
    prepared.headers = {
        { "Content-Type", content_type.empty() ? "application/json" : content_type },
        { "Authorization", "Bearer " + trim_ascii(config.compact_gateway_key) },
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

    const bool allow_private_target = upstream_channel_allows_private_target(config.compact_gateway_base_url);
    try {
        const UpstreamResponse upstream = default_upstream_http_transport(
            prepared, config.proxy_upstream_timeout_seconds * 1000, allow_private_target);
        if (upstream.body.size() > static_cast<size_t>(config.http_max_body_bytes)) {
            return http_response(502, "Bad Gateway", "compact upstream response too large\n",
                                 "text/plain; charset=utf-8", request_id);
        }

        std::string body_bytes = upstream.body;

        if (upstream.status_code >= 400) {
            const long long usage_event_id = parse_long_long(request_id).value_or(0);
            Request usage_request = make_compact_usage_request(*selection, usage_event_id, upstream.status_code, false);
            (void)commit_compact_usage(config, usage_request, nullptr);
            return make_upstream_http_response(upstream.status_code, upstream.headers, std::move(body_bytes));
        }

        const long long usage_event_id = parse_long_long(request_id).value_or(0);
        Request usage_request = make_compact_usage_request(*selection, usage_event_id, upstream.status_code, false);
        if (const auto response_tier = json_string_field_from_body(body_bytes, "service_tier");
            response_tier.has_value()) {
            usage_request.service_tier = *response_tier;
        }
        std::unique_ptr<Request> billing_request;
        if (const auto billing_model = billing_model_for_name(selection->forwarded_model); billing_model.has_value()) {
            billing_request = std::make_unique<Request>(parse_billing_request_from_body(
                GatewayStreamKind::openai_responses, *billing_model, selection->auth.user_id, body_bytes, 1.0,
                selection->route_group_multiplier));
        }
        if (!commit_compact_usage(config, usage_request, billing_request.get())) {
            return http_response(502, "Bad Gateway", "usage commit failed\n", "text/plain; charset=utf-8", request_id);
        }
        return make_upstream_http_response(upstream.status_code, upstream.headers, std::move(body_bytes));
    } catch (const std::exception &) {
        return http_response(502, "Bad Gateway", "compact gateway failed\n", "text/plain; charset=utf-8", request_id);
    }
}

void run_chat_completions_stream(::httplib::Response &res, const ::httplib::Request &req,
                                 const GatewayParsedRequest &parsed, const Config &config, std::string_view request_id,
                                 std::string_view client_ip)
{
    (void)parsed;
    (void)client_ip;
    TokenAuthResult auth_result = authenticated_token(req, config);
    if (!auth_result.auth.has_value()) {
        apply_http_response(http_response(auth_result.status,
                                          auth_result.status == 401 ? "Unauthorized" : "Bad Gateway",
                                          auth_result.message + "\n", "text/plain; charset=utf-8", request_id),
                            res);
        return;
    }
    const TokenAuth &auth = *auth_result.auth;
    if (auth.groups.empty()) {
        apply_http_response(
            http_response(400, "Bad Request", "Token 未配置渠道组\n", "text/plain; charset=utf-8", request_id), res);
        return;
    }

    ProxyGatewayContext gateway(config);
    GatewayRetryBudget budget({ .max_attempts = config.gateway_max_retry_attempts,
                                .max_switches = config.gateway_max_failover_switches,
                                .max_elapsed_ms = config.gateway_max_retry_elapsed_ms });
    std::unordered_set<long long> excluded_channels;
    std::vector<GatewayFailureRecord> failures;
    std::optional<SchedulerChatSelection> selection =
        select_chat_proxy_target_with_scheduler(req.body, gateway, config, auth, excluded_channels, 0);
    if (!selection.has_value()) {
        apply_http_response(http_response(400, "Bad Request",
                                          "chat completions model unavailable on openai-compatible channels\n",
                                          "text/plain; charset=utf-8", request_id),
                            res);
        return;
    }
    if (const auto quota_error = paygo_balance_gate(*gateway.db, auth.user_id, request_id); quota_error.has_value()) {
        apply_http_response(*quota_error, res);
        return;
    }

    const long long usage_event_id = parse_long_long(request_id).value_or(0);
    while (selection.has_value() && budget.can_attempt(!excluded_channels.empty())) {
        const bool switching = !excluded_channels.empty();
        budget.note_attempt(switching);

        const GatewayStreamAttemptExecution executed =
            execute_chat_gateway_stream_attempt(*selection, req, config, request_id);
        if (executed.transport_error.has_value()) {
            const GatewayFailure failure = classify_gateway_transport_failure(executed.transport_error->stage);
            failures.push_back(GatewayFailureRecord{
                .failure = failure,
                .selection = selection,
                .response_headers = {},
                .body_bytes = {},
            });
            gateway.scheduler.report(selection->selection, gateway_failure_to_scheduler_result(failure));
            excluded_channels.insert(selection->selection.channel_id);
            selection = select_chat_proxy_target_with_scheduler(req.body, gateway, config, auth, excluded_channels,
                                                                selection->selection.channel_id);
            continue;
        }

        UpstreamStreamResponse upstream = std::move(executed.result->upstream);
        const int status = upstream.status_code;
        if (status >= 400) {
            GatewayFailure failure = classify_gateway_status_failure(status);
            if (failure.retriable) {
                const std::string error_body = drain_upstream_stream_body(upstream);
                failures.push_back(GatewayFailureRecord{
                    .failure = failure,
                    .selection = selection,
                    .response_headers = {},
                    .body_bytes = error_body,
                });
                gateway.scheduler.report(selection->selection, gateway_failure_to_scheduler_result(failure));
                excluded_channels.insert(selection->selection.channel_id);
                selection = select_chat_proxy_target_with_scheduler(req.body, gateway, config, auth, excluded_channels,
                                                                    selection->selection.channel_id);
                continue;
            }
        }

        const SchedulerChatSelection committed = *selection;
        const auto stream_billing_model = billing_model_for_name(committed.forwarded_model);
        const std::string requested_service_tier = parse_json_string_field(req.body, "service_tier").value_or("");
        std::unique_ptr<Gateway> stream_gateway;
        if (stream_billing_model.has_value()) {
            stream_gateway = make_gateway(GatewayStreamKind::openai_chat, *stream_billing_model, 1.0,
                                          committed.selection.route_group_multiplier);
        }
        apply_upstream_gateway_stream(
            res, status, upstream.headers, std::move(upstream), config, std::move(stream_gateway),
            requested_service_tier, committed.auth.user_id,
            [committed, config, request_id = std::string(request_id), usage_event_id,
             status](const GatewayStreamResult &result) {
                try {
                    const GatewayStreamPump &pump = result.pump;
                    ProxyGatewayContext gateway(config);
                    SchedulerResult scheduler_result;
                    scheduler_result.success = status < 400 && pump.completed && !pump.upstream_error &&
                                               !pump.idle_timeout;
                    std::optional<GatewayFailure> stream_failure;
                    if (!scheduler_result.success) {
                        stream_failure = classify_gateway_stream_failure(pump, status);
                        scheduler_result = gateway_failure_to_scheduler_result(*stream_failure);
                    }
                    gateway.scheduler.report(committed.selection, scheduler_result);

                    if (!scheduler_result.success || !result.billing_request.has_value()) {
                        return;
                    }
                    Request usage_request = make_chat_usage_request(committed, usage_event_id, status, true);
                    usage_request.first_token_latency_ms = pump.first_token_latency_ms;
                    std::unique_ptr<Request> billing_request = std::make_unique<Request>(*result.billing_request);
                    if (!commit_chat_usage(config, usage_request, billing_request.get())) {
                        std::cerr << "stream usage commit failed: " << request_id << '\n';
                    }
                } catch (const std::exception &err) {
                    std::cerr << "stream usage callback failed: " << err.what() << " request_id=" << request_id << '\n';
                }
            });
        return;
    }

    if (!failures.empty()) {
        std::vector<GatewayFailure> raw_failures;
        raw_failures.reserve(failures.size());
        for (const auto &item : failures) {
            raw_failures.push_back(item.failure);
        }
        const GatewayFailureRecord &best = failures[best_gateway_failure_index(raw_failures)];
        const int failure_status = best.failure.status_code >= 400 ? best.failure.status_code : 502;
        const char *failure_reason = failure_status == 429 ? "Too Many Requests" : "Bad Gateway";
        const std::string failure_body = failure_status == 429 && !best.failure.error_message.empty() ?
                                             best.failure.error_message + "\n" :
                                             "proxy upstream failed\n";
        apply_http_response(
            http_response(failure_status, failure_reason, failure_body, "text/plain; charset=utf-8", request_id), res);
        return;
    }

    apply_http_response(
        http_response(502, "Bad Gateway", "proxy upstream failed\n", "text/plain; charset=utf-8", request_id), res);
}

void run_messages_stream(::httplib::Response &res, const ::httplib::Request &req, const GatewayParsedRequest &parsed,
                         const Config &config, std::string_view request_id, std::string_view client_ip)
{
    (void)parsed;
    (void)client_ip;
    TokenAuthResult auth_result = authenticated_token(req, config);
    if (!auth_result.auth.has_value()) {
        apply_http_response(http_response(auth_result.status,
                                          auth_result.status == 401 ? "Unauthorized" : "Bad Gateway",
                                          auth_result.message + "\n", "text/plain; charset=utf-8", request_id),
                            res);
        return;
    }
    const TokenAuth &auth = *auth_result.auth;
    if (auth.groups.empty()) {
        apply_http_response(
            http_response(400, "Bad Request", "Token 未配置渠道组\n", "text/plain; charset=utf-8", request_id), res);
        return;
    }

    const auto selection = select_messages_proxy_target(req.body, config, auth);
    if (!selection.has_value()) {
        apply_http_response(http_response(400, "Bad Request", "messages model unavailable on anthropic channels\n",
                                          "text/plain; charset=utf-8", request_id),
                            res);
        return;
    }

    auto balance_db = make_database(config.db_dsn);
    if (const auto quota_error = paygo_balance_gate(*balance_db, auth.user_id, request_id); quota_error.has_value()) {
        apply_http_response(*quota_error, res);
        return;
    }

    const std::string body = messages_request_body_for_upstream(req.body, *selection);
    SchedulerSelection scheduled{};
    scheduled.channel_id = selection->channel.id;
    scheduled.channel_type = "anthropic";
    scheduled.base_url = selection->channel.base_url;
    scheduled.api_key = selection->channel.api_key;

    auto db = make_database(config.db_dsn);
    const UpstreamRequest downstream = build_gateway_upstream_request(
        req, "/v1/messages", request_id, req.get_header_value("X-Revlm-Client-Ip"), body,
        [](std::string_view lower) { return lower != "authorization" && lower != "x-api-key"; });
    UpstreamExecutor executor;
    UpstreamStreamResponse upstream;
    try {
        const int timeout_ms = config.proxy_upstream_timeout_seconds * 1000;
        const bool allow_private_target = upstream_channel_allows_private_target(scheduled.base_url);
        const UpstreamPreparedRequest prepared = executor.prepare(scheduled, downstream, false, !allow_private_target);
        upstream = default_upstream_http_stream_transport(prepared, timeout_ms, allow_private_target);
    } catch (const std::invalid_argument &) {
        apply_http_response(http_response(502, "Bad Gateway", "proxy upstream unavailable\n",
                                          "text/plain; charset=utf-8", request_id),
                            res);
        return;
    } catch (const std::exception &) {
        apply_http_response(
            http_response(502, "Bad Gateway", "proxy upstream failed\n", "text/plain; charset=utf-8", request_id), res);
        return;
    }

    const int status = upstream.status_code;
    const MessagesProxySelection committed = *selection;
    const long long usage_event_id = parse_long_long(request_id).value_or(0);
    const auto stream_billing_model = billing_model_for_name(committed.forwarded_model);
    const std::string requested_service_tier = parse_json_string_field(req.body, "service_tier").value_or("");
    std::unique_ptr<Gateway> stream_gateway;
    if (stream_billing_model.has_value()) {
        stream_gateway = make_gateway(GatewayStreamKind::anthropics_messages, *stream_billing_model, 1.0,
                                      committed.route_group_multiplier);
    }
    apply_upstream_gateway_stream(
        res, status, upstream.headers, std::move(upstream), config, std::move(stream_gateway), requested_service_tier,
        committed.auth.user_id,
        [committed, config, request_id = std::string(request_id), usage_event_id,
         status](const GatewayStreamResult &result) {
            try {
                const GatewayStreamPump &pump = result.pump;
                if (status >= 400 || !pump.completed || pump.upstream_error || pump.idle_timeout ||
                    !result.billing_request.has_value()) {
                    return;
                }
                Request usage_request = make_messages_usage_request(committed, usage_event_id, status, true);
                usage_request.first_token_latency_ms = pump.first_token_latency_ms;
                std::unique_ptr<Request> billing_request = std::make_unique<Request>(*result.billing_request);
                if (!commit_messages_usage(config, usage_request, billing_request.get())) {
                    std::cerr << "stream usage commit failed: " << request_id << '\n';
                }
            } catch (const std::exception &err) {
                std::cerr << "stream usage callback failed: " << err.what() << " request_id=" << request_id << '\n';
            }
        });
}

void run_responses_compact_stream(::httplib::Response &res, const ::httplib::Request &req,
                                  const GatewayParsedRequest &parsed, const Config &config, std::string_view request_id,
                                  std::string_view client_ip)
{
    (void)parsed;
    (void)client_ip;
    TokenAuthResult auth_result = authenticated_token(req, config);
    if (!auth_result.auth.has_value()) {
        apply_http_response(http_response(auth_result.status,
                                          auth_result.status == 401 ? "Unauthorized" : "Bad Gateway",
                                          auth_result.message + "\n", "text/plain; charset=utf-8", request_id),
                            res);
        return;
    }
    const TokenAuth &auth = *auth_result.auth;
    if (auth.groups.empty()) {
        apply_http_response(
            http_response(400, "Bad Request", "Token 未配置渠道组\n", "text/plain; charset=utf-8", request_id), res);
        return;
    }
    if (trim_ascii(config.compact_gateway_base_url).empty() || trim_ascii(config.compact_gateway_key).empty()) {
        apply_http_response(http_response(502, "Bad Gateway", "compact gateway unavailable\n",
                                          "text/plain; charset=utf-8", request_id),
                            res);
        return;
    }
    const auto selection = select_compact_gateway_target(req.body, config, auth);
    if (!selection.has_value()) {
        apply_http_response(http_response(400, "Bad Request", "compact model unavailable\n",
                                          "text/plain; charset=utf-8", request_id),
                            res);
        return;
    }

    auto balance_db = make_database(config.db_dsn);
    if (const auto quota_error = paygo_balance_gate(*balance_db, auth.user_id, request_id); quota_error.has_value()) {
        apply_http_response(*quota_error, res);
        return;
    }

    const std::string body = compact_request_body_for_gateway(req.body, *selection);
    UpstreamPreparedRequest prepared;
    try {
        prepared.base_url = validate_upstream_base_url(config.compact_gateway_base_url);
        enforce_compact_gateway_guard(prepared.base_url);
    } catch (const std::exception &) {
        apply_http_response(http_response(502, "Bad Gateway", "compact gateway unavailable\n",
                                          "text/plain; charset=utf-8", request_id),
                            res);
        return;
    }
    prepared.method = "POST";
    prepared.url = build_upstream_url(prepared.base_url, "/v1/responses/compact", "");
    const std::string content_type = trim_ascii(req.get_header_value("Content-Type"));
    prepared.headers = {
        { "Content-Type", content_type.empty() ? "application/json" : content_type },
        { "Authorization", "Bearer " + trim_ascii(config.compact_gateway_key) },
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
        const bool allow_private_target = upstream_channel_allows_private_target(config.compact_gateway_base_url);
        upstream = default_upstream_http_stream_transport(prepared, config.proxy_upstream_timeout_seconds * 1000,
                                                          allow_private_target);
    } catch (const std::exception &) {
        apply_http_response(http_response(502, "Bad Gateway", "compact gateway failed\n", "text/plain; charset=utf-8",
                                          request_id),
                            res);
        return;
    }

    const int status = upstream.status_code;
    const CompactGatewaySelection committed = *selection;
    const long long usage_event_id = parse_long_long(request_id).value_or(0);
    const auto stream_billing_model = billing_model_for_name(committed.forwarded_model);
    const std::string requested_service_tier = parse_json_string_field(req.body, "service_tier").value_or("");
    std::unique_ptr<Gateway> stream_gateway;
    if (stream_billing_model.has_value()) {
        stream_gateway = make_gateway(GatewayStreamKind::openai_responses, *stream_billing_model, 1.0,
                                      committed.route_group_multiplier);
    }
    apply_upstream_gateway_stream(
        res, status, upstream.headers, std::move(upstream), config, std::move(stream_gateway), requested_service_tier,
        committed.auth.user_id,
        [committed, config, request_id = std::string(request_id), usage_event_id,
         status](const GatewayStreamResult &result) {
            try {
                const GatewayStreamPump &pump = result.pump;
                if (status >= 400 || !pump.completed || pump.upstream_error || pump.idle_timeout ||
                    !result.billing_request.has_value()) {
                    return;
                }
                Request usage_request = make_compact_usage_request(committed, usage_event_id, status, true);
                usage_request.first_token_latency_ms = pump.first_token_latency_ms;
                std::unique_ptr<Request> billing_request = std::make_unique<Request>(*result.billing_request);
                if (!commit_compact_usage(config, usage_request, billing_request.get())) {
                    std::cerr << "stream compact usage commit failed: " << request_id << '\n';
                }
            } catch (const std::exception &err) {
                std::cerr << "stream compact callback failed: " << err.what() << " request_id=" << request_id << '\n';
            }
        });
}

void apply_http_response(const HttpResponse &response, ::httplib::Response &res)
{
    res.status = response.status;
    if (!response.reason.empty()) {
        res.reason = response.reason;
    }
    for (const Header &header : response.headers) {
        res.set_header(header.name, header.value);
    }
    res.set_content(response.body, response.content_type);
}

} // namespace revlm
