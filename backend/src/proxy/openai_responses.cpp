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
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <httplib.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revlm
{

void OpenaiResponses::finalize(json &json_obj)
{
    // Official: non-stream usage at root; SSE nests under response.usage.
    // cached_tokens is a subset of input_tokens; cache_write_tokens is optional nested.
    const json &root = json_obj;
    const json response = root["response"];
    const json usage = (response.is_object() && response["usage"].is_object()) ? response["usage"] : root["usage"];
    const long long input_tokens = usage["input_tokens"].as_int64().value();
    const long long output_tokens = usage["output_tokens"].as_int64().value();
    const json details = usage["input_tokens_details"];
    const long long cached_tokens = details.is_object() ? details["cached_tokens"].as_int64().value_or(0) : 0;
    const long long cache_write_tokens = details.is_object() ? details["cache_write_tokens"].as_int64().value_or(0) : 0;
    request.usage.input_tokens = static_cast<int>(input_tokens - cached_tokens);
    request.usage.output_tokens = static_cast<int>(output_tokens);
    request.usage.cache_read_tokens = static_cast<int>(cached_tokens);
    request.usage.cache_creation_1h_tokens = 0; // OpenAI has no 1h/5m split
    request.usage.cache_creation_5m_tokens = static_cast<int>(cache_write_tokens);
    const json meta = response.is_object() ? response : root;
    if (const auto tier = meta["service_tier"].as_string(); tier.has_value()) {
        request.upstream.service_tier = normalize_usage_service_tier(std::string_view{ *tier });
    }
    if (const auto model = meta["model"].as_string(); model.has_value() && !model->empty()) {
        request.upstream.model_name = *model;
    }
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

// Official: response.service_tier is actual tier; long-context >272k is 2x for openai-owned.
// Priority unsupported for long context — do not invent 4x.
double tier_multiplier_for(std::string_view response_tier, int official_input_tokens, bool openai_owned)
{
    const std::string_view tier = trim_ascii(response_tier);
    if (tier == "priority") {
        return 2.0;
    }
    if (openai_owned && official_input_tokens > 272000) {
        return 2.0;
    }
    return 1.0;
}

UpstreamRequest build_responses_upstream_request(std::string_view request_id, std::string_view path,
                                                 std::string_view method, std::string request_body_json, bool stream)
{
    // Body already carries service_tier to upstream; no synthetic protocol headers.
    UpstreamRequest downstream;
    downstream.method = std::string{ method };
    downstream.path = std::string{ path };
    downstream.body = std::move(request_body_json);
    downstream.headers = {
        { "Content-Type", "application/json" },
        { "Accept", stream ? "text/event-stream" : "application/json" },
        { "X-Request-Id", std::string{ request_id } },
    };
    return downstream;
}

ProxyUpstreamResponse perform_upstream_request(long long channel_id, const ProxyRequest &pr, std::string_view path,
                                               std::string_view method, std::string request_body_json)
{
    UpstreamRequest downstream =
        build_responses_upstream_request(pr.request_id, path, method, std::move(request_body_json), false);

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

UpstreamSession open_upstream_stream_session(long long channel_id, const ProxyRequest &pr, std::string_view path,
                                             std::string_view method, std::string request_body_json)
{
    UpstreamRequest downstream =
        build_responses_upstream_request(pr.request_id, path, method, std::move(request_body_json), true);

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
                                const ProxyUpstreamResponse &upstream, int latency_ms, ProxyRequest &pr)
{
    if (upstream.status >= 400) {
        responses_from_upstream(res, request_id, upstream);
        return;
    }

    // Upstream succeeded → always forward body. Pricing is best-effort from catalog.
    pr.upstream.channel_id = channel_id;
    pr.upstream.status_code = upstream.status;
    pr.is_stream = false;
    pr.upstream.latency_ms = std::max(latency_ms, 0);
    pr.request_id = request_id;
    pr.upstream.response_id = upstream.response_id;
    pr.upstream.tier_multiplier = 1.0;
    parse_billing_request_from_body(pr, GatewayStreamKind::openai_responses, upstream.body);
    if (const auto channel = ChannelStore::instance().find_channel(channel_id); channel.has_value()) {
        pr.upstream.channel_multiplier = channel->price_multiplier;
        const Model *model = channel->find_model(pr.upstream.model_name);
        if (model != nullptr) {
            fill_pricing_from_model(pr.upstream.pricing, *model);
        }
        pr.upstream.tier_multiplier = tier_multiplier_for(pr.upstream.service_tier,
                                                          pr.usage.input_tokens + pr.usage.cache_read_tokens,
                                                          model != nullptr && model->owned_by == "openai");
    }
    responses_from_upstream(res, request_id, upstream);
}

void stream_upstream_session_to_httplib(::httplib::Response &res, UpstreamSession session, std::string_view request_id,
                                        ProxyRequest usage, double route_group_multiplier,
                                        std::function<void(ProxyRequest &usage, int first_token_latency_ms)> on_complete)
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
        ProxyRequest usage;
        double channel_multiplier = 1.0;
        std::function<void(ProxyRequest &, int)> on_complete;
    };
    auto shared = std::make_shared<Shared>();
    shared->session = std::move(session);
    shared->usage = std::move(usage);
    shared->usage.upstream.channel_multiplier = route_group_multiplier;
    shared->channel_multiplier = route_group_multiplier;
    shared->on_complete = std::move(on_complete);

    const int idle_timeout_ms = std::max(1000, config().proxy_upstream_timeout_seconds * 1000);
    res.set_chunked_content_provider(content_type, [shared, idle_timeout_ms](size_t offset,
                                                                             ::httplib::DataSink &sink) mutable {
        if (offset != 0) {
            return false;
        }
        auto stream_gateway = make_gateway(GatewayStreamKind::openai_responses, shared->usage);
        const GatewayStreamResult gateway_result = pump_gateway_stream(
            shared->session.stream.read,
            [&sink](std::string_view data) { return sink.write(data.data(), data.size()); }, shared->session.head.body,
            idle_timeout_ms, shared->session.stream.poll_fd, *stream_gateway);
        const int session_status = shared->session.head.status;
        shared->session.close_stream();
        if (shared->on_complete && gateway_result.pump.saw_usage && session_status < 400) {
            shared->on_complete(shared->usage, gateway_result.pump.first_token_latency_ms);
        }
        sink.done();
        return true;
    });
}

bool stream_upstream_session_to_client(UpstreamSession &session, const ClientWriter &write_client,
                                       std::string_view request_id, ProxyRequest &usage,
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
    auto gateway = make_gateway(GatewayStreamKind::openai_responses, usage);
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

ResponsesProxyResult handle_responses_proxy_request(ProxyRequest &pr, ::httplib::Response &res,
                                                    const ResponsesProxyExecuteOptions &options)
{
    const std::string &method = pr.http.method;
    const std::string &path = pr.http.path;
    const std::string &request_id = pr.request_id;
    const long long channel_group_id = pr.auth.channel_group_id;

    if (method != "POST") {
        write_upstream(res, 405, serialize(json_error_body("method not allowed")),
                       { { "X-Request-Id", std::string{ request_id } },
                         { "Content-Type", "application/json; charset=utf-8" } });
        return {};
    }

    const std::string &body = pr.http.body;
    const bool stream_requested = pr.is_stream;

    const auto request_started_at = Clock::now();
    const auto elapsed_latency_ms = [&request_started_at]() {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - request_started_at).count());
    };

    try {
        pr.http.body.clear();
        pr.http.body.shrink_to_fit();

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
            tried = true;
            const long long channel_id = channel.id;
            const double route_mult = channel.price_multiplier;

            std::optional<UpstreamSession> stream_session;
            ProxyUpstreamResponse upstream;
            bool attempt_failed = false;

            try {
                if (stream_requested && stream_sink_ready) {
                    UpstreamSession session = open_upstream_stream_session(channel_id, pr, path, method, body);
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
                        pr.upstream.channel_id = channel_id;
                        pr.upstream.status_code = upstream.status;
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
                    upstream = perform_upstream_request(channel_id, pr, path, method, body);
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
                        pr.upstream.channel_id = channel_id;
                        pr.upstream.status_code = upstream.status;
                        attempt_failed = true;
                    }
                }
            } catch (const std::exception &err) {
                last_status = 502;
                last_body = serialize(json_error_body(err.what()));
                last_headers = { { "X-Request-Id", request_id } };
                pr.upstream.channel_id = channel_id;
                pr.upstream.status_code = 502;
                attempt_failed = true;
            }

            if (attempt_failed) {
                group->next_channel();
                continue;
            }

            if (stream_session.has_value()) {
                UpstreamSession &session = *stream_session;
                const int stream_status = session.head.status;
                pr.upstream.channel_id = channel_id;
                pr.upstream.status_code = stream_status;
                pr.upstream.channel_multiplier = route_mult;
                pr.is_stream = true;
                pr.request_id = request_id;
                pr.upstream.response_id = session.head.response_id;

                auto finish_stream_billing = [&](ProxyRequest &stream_request, int first_token_latency_ms) {
                    // finalize() already filled usage/model/service_tier from official SSE JSON.
                    const Model *model = channel.find_model(stream_request.upstream.model_name);
                    if (model != nullptr) {
                        fill_pricing_from_model(stream_request.upstream.pricing, *model);
                    }
                    stream_request.upstream.tier_multiplier =
                        tier_multiplier_for(stream_request.upstream.service_tier,
                                            stream_request.usage.input_tokens + stream_request.usage.cache_read_tokens,
                                            model != nullptr && model->owned_by == "openai");
                    stream_request.upstream.latency_ms = elapsed_latency_ms();
                    stream_request.upstream.first_token_latency_ms =
                        std::min(std::max(first_token_latency_ms, 0), std::max(stream_request.upstream.latency_ms, 0));
                    stream_request.upstream.channel_multiplier = route_mult;
                };
                if (options.stream_response != nullptr) {
                    ProxyRequest stream_usage = pr;
                    stream_upstream_session_to_httplib(
                        *options.stream_response, std::move(session), request_id, std::move(stream_usage), route_mult,
                        [&](ProxyRequest &stream_request, int first_token_latency_ms) {
                            if (stream_status >= 400 || !options.on_usage) {
                                return;
                            }
                            finish_stream_billing(stream_request, first_token_latency_ms);
                            options.on_usage(stream_request);
                        });
                    return ResponsesProxyResult{
                        .handled_stream = true,
                        .stream_status = stream_status,
                    };
                }
                std::optional<std::string> upstream_response_model;
                long long response_bytes = 0;
                int first_token_latency_ms = 0;
                bool had_usage = false;
                if (!stream_upstream_session_to_client(session, write_client, request_id, pr, upstream_response_model,
                                                       response_bytes, first_token_latency_ms, had_usage)) {
                    throw std::runtime_error("stream pump failed");
                }
                if (session.head.status < 400 && had_usage) {
                    finish_stream_billing(pr, first_token_latency_ms);
                    (void)commit_proxy_usage(pr);
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

            finalize_non_stream_result(res, request_id, channel_id, upstream, elapsed_latency_ms(), pr);
            return {};
        } while (group->pointer != start);

        if (!tried) {
            write_upstream(res, 400, serialize(json_error_body("no available openai-compatible channel")),
                           { { "X-Request-Id", std::string{ request_id } },
                             { "Content-Type", "application/json; charset=utf-8" } });
            return {};
        }
        write_upstream(res, last_status, std::move(last_body), last_headers);
        return {};
    } catch (const std::exception &err) {
        write_upstream(res, 502, serialize(json_error_body(err.what())),
                       { { "X-Request-Id", std::string{ request_id } },
                         { "Content-Type", "application/json; charset=utf-8" } });
        return {};
    }
}

} // namespace revlm
