#include "proxy/openai_responses.hpp"

#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "config/config.hpp"
#include "models/models.hpp"
#include "proxy/upstream.hpp"
#include "proxy/gateway.hpp"
#include "request/request.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <algorithm>
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revlm
{
namespace
{

long long json_int64_or(const json &obj, std::string_view key, long long fallback = 0)
{
    if (!obj.is_object() || !obj.contains(key)) {
        return fallback;
    }
    return static_cast<const json &>(obj)[key].as_int64().value_or(fallback);
}

} // namespace

void OpenaiResponses::finalize(json &json_obj)
{
    const json &root = json_obj;
    json usage_parent = root;
    const json response = root["response"];
    if (response.is_object()) {
        usage_parent = response;
    }
    const json usage = usage_parent["usage"];
    if (!usage.is_object()) {
        return;
    }
    const long long input_tokens = json_int64_or(usage, "input_tokens");
    const long long output_tokens = json_int64_or(usage, "output_tokens");

    long long cached = json_int64_or(usage, "cache_read_input_tokens");
    const json details = usage["input_tokens_details"];
    if (details.is_object()) {
        const long long from_details = json_int64_or(details, "cached_tokens");
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

struct ResponseHead {
    int status = 502;
    std::string body;
    std::string content_type;
    std::string response_id;
};

struct ProxyUpstreamResponse {
    int status = 502;
    std::string body;
    std::string content_type;
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

json json_error_body(std::string_view message)
{
    return json{ { "error", json{ { "message", std::string{ message } } } } };
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

bool channel_ok_for_openai_responses(const Channel &channel)
{
    return channel.status && channel.type == "openai_compatible" && !trim_ascii(channel.api_key).empty();
}

std::optional<ChannelGroup> load_responses_channel_group(long long channel_group_id)
{
    if (channel_group_id <= 0) {
        return std::nullopt;
    }
    ChannelGroup group = ChannelGroupStore::instance().get_channel_group_by_id(channel_group_id);
    if (group.id <= 0 || !group.status || group.channels.empty()) {
        return std::nullopt;
    }
    if (group.pointer < 0 || group.pointer >= static_cast<int>(group.channels.size())) {
        group.pointer = 0;
    }
    return group;
}

void responses_from_upstream(::httplib::Response &res, std::string_view request_id,
                             const ProxyUpstreamResponse &upstream)
{
    std::vector<UpstreamHeader> headers{ { "X-Request-Id", std::string{ request_id } } };
    if (!upstream.response_id.empty()) {
        headers.push_back({ "X-Response-Id", upstream.response_id });
    }
    if (!upstream.content_type.empty()) {
        headers.push_back({ "Content-Type", upstream.content_type });
    }
    write_upstream(res, upstream.status, upstream.body, headers);
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
    const std::optional<std::string> response_tier = parse_json_string_field(body, "service_tier");
    const std::string_view effective_tier = response_tier.has_value() ? std::string_view(*response_tier) :
                                                                        requested_service_tier;
    usage.tier_multiplier = tier_multiplier_for(*model, requested_service_tier, effective_tier, usage.input_tokens);
    if (response_tier.has_value()) {
        usage.service_tier = normalize_usage_service_tier(std::string_view(*response_tier));
    }
}

UpstreamRequest build_responses_upstream_request(std::string_view request_id, std::string_view path,
                                                 std::string_view method, std::string request_body_json,
                                                 std::string_view service_tier, bool stream)
{
    // Synthetic upstream headers only; client envelope header is intentionally ignored.
    UpstreamRequest downstream;
    downstream.method = std::string{ method };
    downstream.path = std::string{ path };
    downstream.body = std::move(request_body_json);
    downstream.headers = {
        { "Content-Type", "application/json" },
        { "Accept", stream ? "text/event-stream" : "application/json" },
        { "X-Request-Id", std::string{ request_id } },
    };
    if (!trim_ascii(service_tier).empty()) {
        downstream.headers.push_back({ "X-Revlm-Service-Tier", std::string{ service_tier } });
    }
    return downstream;
}

ProxyUpstreamResponse perform_upstream_request(long long channel_id, const json &envelope, std::string_view path,
                                               std::string_view method, std::string request_body_json,
                                               std::string_view service_tier)
{
    UpstreamRequest downstream = build_responses_upstream_request(envelope["request_id"].as_string().value_or(""), path,
                                                                  method, std::move(request_body_json), service_tier,
                                                                  false);

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
        .body = std::move(executed.result->response.body),
        .content_type = content_type,
        .response_id = upstream_response_id_from_headers(executed.result->response.headers),
    };
}

UpstreamSession open_upstream_stream_session(long long channel_id, const json &envelope, std::string_view path,
                                             std::string_view method, std::string request_body_json,
                                             std::string_view service_tier)
{
    UpstreamRequest downstream = build_responses_upstream_request(envelope["request_id"].as_string().value_or(""), path,
                                                                  method, std::move(request_body_json), service_tier,
                                                                  true);

    ScheduledUpstreamStreamExecution executed = open_scheduled_upstream_stream(channel_id, std::move(downstream));
    if (!executed.result.has_value()) {
        throw std::runtime_error(executed.transport_error->message.empty() ? "upstream unavailable" :
                                                                             executed.transport_error->message);
    }
    UpstreamStreamResponse stream_response = std::move(*executed.result);

    std::string content_type;
    for (const UpstreamHeader &header : stream_response.headers) {
        if (lowercase_ascii(header.name) == "content-type") {
            content_type = header.value;
            break;
        }
    }

    UpstreamSession session;
    session.head.status = stream_response.status_code;
    session.head.body = std::move(stream_response.initial_body);
    session.head.content_type = content_type;
    session.head.response_id = upstream_response_id_from_headers(stream_response.headers);
    session.stream = std::move(stream_response.stream);
    return session;
}

void finalize_non_stream_result(::httplib::Response &res, std::string_view request_id, long long channel_id,
                                std::string_view model_name, std::string_view requested_service_tier,
                                const ProxyUpstreamResponse &upstream, int latency_ms, Request &usage)
{
    if (upstream.status >= 400) {
        responses_from_upstream(res, request_id, upstream);
        return;
    }

    const auto channel = ChannelStore::instance().find_channel(channel_id);
    const Model *billing_model = channel.has_value() ? channel->find_model(model_name) : nullptr;
    if (billing_model == nullptr) {
        write_upstream(res, 502, serialize(json_error_body("usage commit failed")),
                       { { "X-Request-Id", std::string{ request_id } },
                         { "Content-Type", "application/json; charset=utf-8" } });
        return;
    }

    usage.channel_id = channel_id;
    usage.status_code = upstream.status;
    usage.is_stream = false;
    usage.latency_ms = std::max(latency_ms, 0);
    assign_request_correlation(usage, request_id, upstream.response_id);
    apply_responses_billing_from_json(usage, billing_model, upstream.body, requested_service_tier,
                                      channel->price_multiplier);
    usage.usd = usage.solve_price();
    usage.pricing_model = nullptr;
    responses_from_upstream(res, request_id, upstream);
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
    std::vector<UpstreamHeader> headers{ { "X-Request-Id", std::string{ request_id } } };
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

ResponsesProxyResult handle_responses_proxy_request(json req, ::httplib::Response &res, Request &usage,
                                                    const ResponsesProxyExecuteOptions &options)
{
    const std::string method = req["method"].as_string().value_or("");
    const std::string path = req["path"].as_string().value_or("");
    const std::string request_id = req["request_id"].as_string().value_or("");
    const long long channel_group_id = req["channel_group_id"].as_int64().value_or(0);

    if (method != "POST") {
        write_upstream(res, 405, serialize(json_error_body("method not allowed")),
                       { { "X-Request-Id", std::string{ request_id } },
                         { "Content-Type", "application/json; charset=utf-8" } });
        return {};
    }

    std::string body = req["body"].as_string().value_or("");
    const bool stream_requested = req["is_stream"].as_bool().value_or(false);
    const std::optional<std::string> model_from_body = parse_json_string_field(body, "model");
    if (!model_from_body.has_value()) {
        write_upstream(res, 400, serialize(json_error_body("model is required")),
                       { { "X-Request-Id", std::string{ request_id } },
                         { "Content-Type", "application/json; charset=utf-8" } });
        return {};
    }

    const auto request_started_at = Clock::now();
    const auto elapsed_latency_ms = [&request_started_at]() {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - request_started_at).count());
    };

    try {
        const std::string model_name = trim_ascii(*model_from_body);
        if (model_name.empty()) {
            throw std::invalid_argument("model is required");
        }

        const std::string requested_service_tier =
            normalize_service_tier_request(parse_json_string_field(body, "service_tier"));

        auto group = load_responses_channel_group(channel_group_id);
        if (!group.has_value()) {
            write_upstream(res, 400, serialize(json_error_body("channel group unavailable")),
                           { { "X-Request-Id", std::string{ request_id } },
                             { "Content-Type", "application/json; charset=utf-8" } });
            return {};
        }

        ClientWriter write_client = options.write_client;
        if (!write_client && options.client_fd >= 0) {
            write_client = client_writer_from_fd(options.client_fd);
        }
        const bool stream_sink_ready = write_client || options.stream_response != nullptr;

        const int start = group->pointer;
        int last_status = 502;
        std::string last_body = serialize(json_error_body("proxy upstream failed"));
        std::vector<UpstreamHeader> last_headers{ { "X-Request-Id", request_id } };
        bool tried = false;

        do {
            Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
            if (!channel_ok_for_openai_responses(channel)) {
                group->next_channel();
                continue;
            }
            const Model *model = channel.find_model(model_name);
            if (model == nullptr) {
                group->next_channel();
                continue;
            }
            tried = true;
            const long long channel_id = channel.id;
            const double route_mult = channel.price_multiplier;

            std::optional<UpstreamSession> stream_session;
            ProxyUpstreamResponse upstream;
            bool attempt_failed = false;

            try {
                if (stream_requested && stream_sink_ready) {
                    UpstreamSession session =
                        open_upstream_stream_session(channel_id, req, path, method, body, requested_service_tier);
                    if (session.head.status >= 400) {
                        upstream = ProxyUpstreamResponse{
                            .status = session.head.status,
                            .body = session.head.body + read_remaining_stream(session.stream),
                            .content_type = session.head.content_type,
                            .response_id = session.head.response_id,
                        };
                        last_status = upstream.status;
                        last_body = upstream.body;
                        last_headers = { { "X-Request-Id", request_id } };
                        if (!upstream.response_id.empty()) {
                            last_headers.push_back({ "X-Response-Id", upstream.response_id });
                        }
                        if (!upstream.content_type.empty()) {
                            last_headers.push_back({ "Content-Type", upstream.content_type });
                        }
                        usage.channel_id = channel_id;
                        usage.status_code = upstream.status;
                        attempt_failed = true;
                    } else if (!is_sse_content_type(session.head.content_type)) {
                        upstream = ProxyUpstreamResponse{
                            .status = session.head.status,
                            .body = session.head.body + read_remaining_stream(session.stream),
                            .content_type = session.head.content_type,
                            .response_id = session.head.response_id,
                        };
                    } else {
                        stream_session = std::move(session);
                    }
                } else {
                    upstream = perform_upstream_request(channel_id, req, path, method, body, requested_service_tier);
                    if (upstream.status >= 400) {
                        last_status = upstream.status;
                        last_body = upstream.body;
                        last_headers = { { "X-Request-Id", request_id } };
                        if (!upstream.response_id.empty()) {
                            last_headers.push_back({ "X-Response-Id", upstream.response_id });
                        }
                        if (!upstream.content_type.empty()) {
                            last_headers.push_back({ "Content-Type", upstream.content_type });
                        }
                        usage.channel_id = channel_id;
                        usage.status_code = upstream.status;
                        attempt_failed = true;
                    }
                }
            } catch (const std::invalid_argument &) {
                throw;
            } catch (const std::exception &err) {
                last_status = 502;
                last_body = serialize(json_error_body(err.what()));
                last_headers = { { "X-Request-Id", request_id } };
                usage.channel_id = channel_id;
                usage.status_code = 502;
                attempt_failed = true;
            }

            if (attempt_failed) {
                group->next_channel();
                continue;
            }

            if (stream_session.has_value()) {
                UpstreamSession &session = *stream_session;
                const Model *billing_model = model;
                const int stream_status = session.head.status;
                const std::string stream_response_id = session.head.response_id;
                const std::optional<std::string> head_response_service_tier =
                    parse_json_string_field(session.head.body, "service_tier");

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
                    stream_request.usd = stream_request.solve_price();
                    options.on_usage(stream_request);
                    stream_request.pricing_model = nullptr;
                };
                if (options.stream_response != nullptr) {
                    Request stream_usage = usage;
                    stream_upstream_session_to_httplib(*options.stream_response, std::move(session), request_id,
                                                       std::move(stream_usage), requested_service_tier, route_mult,
                                                       [&](Request &stream_request, int first_token_latency_ms) {
                                                           emit_stream_usage(stream_request, first_token_latency_ms);
                                                       });
                    return ResponsesProxyResult{
                        .handled_stream = true,
                        .stream_status = stream_status,
                    };
                }
                apply_responses_billing_from_json(usage, billing_model, session.head.body, requested_service_tier,
                                                  route_mult);
                std::optional<std::string> upstream_response_model =
                    parse_json_string_field(session.head.body, "model");
                long long response_bytes = 0;
                int first_token_latency_ms = 0;
                bool had_usage = false;
                if (!stream_upstream_session_to_client(session, write_client, request_id, usage,
                                                       upstream_response_model, response_bytes, first_token_latency_ms,
                                                       had_usage)) {
                    throw std::runtime_error("stream pump failed");
                }
                if (session.head.status < 400 && had_usage && usage.pricing_model != nullptr) {
                    usage.latency_ms = elapsed_latency_ms();
                    usage.first_token_latency_ms =
                        std::min(std::max(first_token_latency_ms, 0), std::max(usage.latency_ms, 0));
                    usage.usd = usage.solve_price();
                    if (!commit_proxy_usage(usage)) {
                        throw std::runtime_error("usage commit failed");
                    }
                    usage.pricing_model = nullptr;
                }
                return ResponsesProxyResult{
                    .handled_stream = true,
                    .stream_status = session.head.status,
                };
            }

            if (path == "/v1/responses/input_tokens") {
                responses_from_upstream(res, request_id, upstream);
                return {};
            }

            finalize_non_stream_result(res, request_id, channel_id, model->name, requested_service_tier, upstream,
                                       elapsed_latency_ms(), usage);
            return {};
        } while (group->pointer != start);

        if (!tried) {
            write_upstream(res, 400,
                           serialize(json_error_body("responses model unavailable on openai-compatible channels")),
                           { { "X-Request-Id", std::string{ request_id } },
                             { "Content-Type", "application/json; charset=utf-8" } });
            return {};
        }
        write_upstream(res, last_status, std::move(last_body), last_headers);
        return {};
    } catch (const std::invalid_argument &err) {
        write_upstream(res, 400, serialize(json_error_body(err.what())),
                       { { "X-Request-Id", std::string{ request_id } },
                         { "Content-Type", "application/json; charset=utf-8" } });
        return {};
    } catch (const std::exception &err) {
        write_upstream(res, 502, serialize(json_error_body(err.what())),
                       { { "X-Request-Id", std::string{ request_id } },
                         { "Content-Type", "application/json; charset=utf-8" } });
        return {};
    }
}

} // namespace revlm
