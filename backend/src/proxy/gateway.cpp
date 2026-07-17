#include "proxy/gateway.hpp"

#include "auth/security.hpp"
#include "channels/channels.hpp"
#include "config/config.hpp"
#include "models/models.hpp"
#include "models/quota.hpp"
#include "proxy/upstream.hpp"
#include "proxy/anthropics_messages.hpp"
#include "proxy/openai_chat.hpp"
#include "proxy/openai_responses.hpp"
#include "request/request.hpp"
#include "server/http_server.hpp"
#include "users/users.hpp"
#include "util/json.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
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
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
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
    res.set_content(serialize(response.body), response.content_type);
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
    return http_response(402, "Payment Required", json{ { "error", json{ { "message", "insufficient balance" } } } },
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

std::string remove_json_field(std::string_view json_text, std::string_view field_name)
{
    auto value = json::parse(json_text);
    if (!value || !value->is_object()) {
        return std::string{ json_text };
    }
    value->erase(field_name);
    return value->dump();
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

namespace
{

bool send_all_fd(int fd, std::string_view data)
{
    while (!data.empty()) {
        const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

} // namespace

ClientWriter client_writer_from_fd(int fd)
{
    return [fd](std::string_view data) { return send_all_fd(fd, data); };
}

bool is_sse_content_type(std::string_view content_type)
{
    const std::string normalized = lowercase_ascii(content_type);
    return normalized.find("text/event-stream") != std::string::npos;
}

HttpResponse make_upstream_http_response(int status, std::string body, std::vector<Header> headers)
{
    HttpResponse out;
    out.status = status;
    out.reason = (status >= 200 && status < 300) ? "OK" : "Upstream";
    out.content_type = "application/json; charset=utf-8";
    if (auto parsed = json::parse(body)) {
        out.body = std::move(*parsed);
    } else {
        out.body = json(std::move(body));
    }
    out.headers.reserve(headers.size());
    for (Header &header : headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "connection" || lower == "transfer-encoding" || lower == "content-length") {
            continue;
        }
        if (lower == "content-type") {
            out.content_type = std::move(header.value);
            continue;
        }
        out.headers.push_back(std::move(header));
    }
    return out;
}

std::string read_remaining_stream(const UpstreamReadHandle &stream)
{
    if (!stream.read) {
        return {};
    }
    std::string out;
    char buffer[8192];
    for (;;) {
        const ssize_t n = stream.read(buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
        out.append(buffer, static_cast<size_t>(n));
    }
    return out;
}

std::string drain_upstream_stream_body(UpstreamStreamResponse &upstream)
{
    std::string out = std::move(upstream.initial_body);
    out += read_remaining_stream(upstream.stream);
    if (upstream.stream.close) {
        upstream.stream.close();
    }
    return out;
}

std::string format_upstream_proxy_response_headers(int status_code, const std::vector<UpstreamHeader> &headers,
                                                   size_t body_size)
{
    std::string text = "HTTP/1.1 " + std::to_string(status_code);
    text += status_code >= 200 && status_code < 300 ? " OK\r\n" : " Upstream\r\n";
    for (const auto &header : headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "connection" || lower == "transfer-encoding" || lower == "content-length") {
            continue;
        }
        text += header.name + ": " + header.value + "\r\n";
    }
    text += "Content-Length: " + std::to_string(body_size) + "\r\n";
    text += "Connection: close\r\n\r\n";
    return text;
}

std::string build_synthetic_stream_response_head(int status, std::string_view content_type,
                                                 const std::vector<Header> &headers)
{
    std::ostringstream out;
    out << "HTTP/1.1 " << status << (status >= 200 && status < 300 ? " OK" : " Bad Gateway") << "\r\n"
        << "Content-Type: " << (content_type.empty() ? "text/event-stream; charset=utf-8" : content_type) << "\r\n";
    for (const Header &header : headers) {
        out << header.name << ": " << header.value << "\r\n";
    }
    out << "Connection: close\r\n\r\n";
    return out.str();
}

namespace
{

constexpr size_t kMaxSseLineBytes = 64 * 1024;
constexpr size_t kMaxSseEventBytes = 256 * 1024;
constexpr size_t kFlushBytes = 1024;
constexpr int kDisconnectDrainTimeoutMs = 1500;

struct SseEvent {
    std::string data;
    bool done = false;
};

class SseReader {
public:
    bool consume(std::string_view chunk, std::vector<SseEvent> &out)
    {
        if (!ok_) {
            return false;
        }
        for (char ch : chunk) {
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                if (!push_line(out)) {
                    return false;
                }
                continue;
            }
            line_.push_back(ch);
            if (line_.size() > kMaxSseLineBytes) {
                ok_ = false;
                return false;
            }
        }
        return ok_;
    }

    bool finish()
    {
        if (!ok_) {
            return false;
        }
        if (event_open_) {
            data_.clear();
            event_open_ = false;
        }
        line_.clear();
        return true;
    }

private:
    bool push_line(std::vector<SseEvent> &out)
    {
        std::string line;
        line.swap(line_);
        if (line.empty()) {
            if (event_open_) {
                const bool done = trim_ascii(data_) == "[DONE]";
                out.push_back(SseEvent{ std::move(data_), done });
                data_.clear();
                event_bytes_ = 0;
                event_open_ = false;
            }
            return true;
        }

        event_open_ = true;
        event_bytes_ += line.size() + 1;
        if (event_bytes_ > kMaxSseEventBytes) {
            ok_ = false;
            return false;
        }
        if (line[0] == ':') {
            return true;
        }
        const size_t colon = line.find(':');
        std::string_view field = line;
        std::string_view value;
        if (colon != std::string::npos) {
            field = std::string_view{ line }.substr(0, colon);
            value = std::string_view{ line }.substr(colon + 1);
            if (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
        }
        if (field == "data") {
            if (!data_.empty()) {
                data_.push_back('\n');
            }
            data_.append(value.data(), value.size());
        }
        return true;
    }

    size_t event_bytes_ = 0;
    std::string line_;
    std::string data_;
    bool event_open_ = false;
    bool ok_ = true;
};

bool contains_usage_object(const json &value)
{
    if (value.is_object()) {
        const json usage = value["usage"];
        if (usage.is_object()) {
            return true;
        }
        for (const auto &key : value.keys()) {
            if (contains_usage_object(value[key])) {
                return true;
            }
        }
        return false;
    }
    if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (contains_usage_object(value[i])) {
                return true;
            }
        }
    }
    return false;
}

std::optional<std::string> find_first_model(const json &value)
{
    if (value.is_object()) {
        for (const auto &key : value.keys()) {
            const json child = value[key];
            if (key == "model" && child.is_string()) {
                if (const auto model = child.as_string(); model.has_value() && !model->empty()) {
                    return *model;
                }
            }
            if (const auto nested = find_first_model(child)) {
                return nested;
            }
        }
        return std::nullopt;
    }
    if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (const auto nested = find_first_model(value[i])) {
                return nested;
            }
        }
    }
    return std::nullopt;
}

int poll_readable(int fd, int timeout_ms)
{
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN | POLLERR | POLLHUP;
    for (;;) {
        const int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc >= 0) {
            return rc;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

void handle_sse_event(const SseEvent &event, const std::chrono::steady_clock::time_point &started_at,
                      GatewayStreamPump &pump, Gateway &gateway)
{
    if (event.done) {
        pump.completed = true;
        return;
    }
    const auto doc = json::parse(trim_ascii(event.data));
    if (!doc || !doc->is_object()) {
        return;
    }
    if (pump.first_token_latency_ms == 0) {
        pump.first_token_latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at)
                .count());
    }
    const json &root = *doc;
    const json type_field = root["type"];
    if (type_field.is_string()) {
        const std::string type_str = type_field.as_string().value_or("");
        if (type_str == "message_stop" || type_str == "response.completed") {
            pump.completed = true;
        }
    }
    if (const auto model = find_first_model(*doc)) {
        pump.model = *model;
    }
    if (contains_usage_object(*doc)) {
        json mutable_doc = *doc;
        gateway.finalize(mutable_doc);
        pump.saw_usage = true;
    }
}

} // namespace

std::unique_ptr<Gateway> make_gateway(GatewayStreamKind kind, const Model *model, double tier_multiplier,
                                      double channel_multiplier, Request &usage)
{
    switch (kind) {
    case GatewayStreamKind::openai_chat:
        return std::make_unique<OpenaiChatCompletion>(usage, model, tier_multiplier, channel_multiplier);
    case GatewayStreamKind::openai_responses:
        return std::make_unique<OpenaiResponses>(usage, model, tier_multiplier, channel_multiplier);
    case GatewayStreamKind::anthropics_messages:
        return std::make_unique<AnthropicsMessages>(usage, model, tier_multiplier, channel_multiplier);
    }
    return nullptr;
}

void parse_billing_request_from_body(Request &out, GatewayStreamKind kind, std::string_view body)
{
    if (out.pricing_model == nullptr) {
        return;
    }
    auto gateway = make_gateway(kind, out.pricing_model, out.tier_multiplier, out.channel_multiplier, out);
    if (gateway == nullptr) {
        return;
    }
    const auto doc = json::parse(trim_ascii(body));
    if (!doc || !doc->is_object()) {
        return;
    }
    json parsed = *doc;
    gateway->finalize(parsed);
}

GatewayStreamResult pump_gateway_stream(const std::function<ssize_t(char *, size_t)> &read_chunk,
                                        const std::function<bool(std::string_view)> &write_to_client,
                                        std::string_view initial_body, int idle_timeout_ms, int poll_fd,
                                        Gateway &gateway)
{
    GatewayStreamResult out;
    SseReader reader;
    std::vector<SseEvent> events;
    events.reserve(8);
    std::string pending_send;
    pending_send.reserve(kFlushBytes);
    const auto started_at = std::chrono::steady_clock::now();

    auto ingest = [&](std::string_view bytes) -> bool {
        out.pump.response_bytes += bytes.size();
        if (!out.pump.client_disconnected) {
            pending_send.append(bytes.data(), bytes.size());
            if (pending_send.size() >= kFlushBytes) {
                if (!write_to_client(pending_send)) {
                    out.pump.client_disconnected = true;
                }
                pending_send.clear();
            }
        }
        events.clear();
        if (!reader.consume(bytes, events)) {
            out.pump.upstream_error = true;
            return false;
        }
        for (const SseEvent &event : events) {
            handle_sse_event(event, started_at, out.pump, gateway);
        }
        return true;
    };

    if (!initial_body.empty() && !ingest(initial_body)) {
        if (!pending_send.empty() && !out.pump.client_disconnected) {
            (void)write_to_client(pending_send);
        }
        return out;
    }

    char buffer[8192];
    for (;;) {
        if (poll_fd >= 0) {
            const int timeout_ms = out.pump.client_disconnected ? kDisconnectDrainTimeoutMs : idle_timeout_ms;
            const int polled = poll_readable(poll_fd, timeout_ms);
            if (polled == 0) {
                if (!out.pump.client_disconnected) {
                    out.pump.idle_timeout = true;
                }
                break;
            }
            if (polled < 0) {
                out.pump.upstream_error = true;
                break;
            }
        }

        const ssize_t n = read_chunk(buffer, sizeof(buffer));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                if (!out.pump.client_disconnected) {
                    out.pump.idle_timeout = true;
                }
                break;
            }
            out.pump.upstream_error = true;
            break;
        }
        if (!ingest(std::string_view{ buffer, static_cast<size_t>(n) })) {
            break;
        }
        if (out.pump.client_disconnected && out.pump.completed) {
            break;
        }
    }

    if (!reader.finish()) {
        out.pump.upstream_error = true;
    }
    if (!pending_send.empty() && !out.pump.client_disconnected) {
        (void)write_to_client(pending_send);
    }

    return out;
}

void apply_upstream_gateway_stream(::httplib::Response &res, int status, const std::vector<UpstreamHeader> &headers,
                                   UpstreamStreamResponse upstream, Request usage,
                                   std::function<std::unique_ptr<Gateway>(Request &)> make_gateway_for_usage,
                                   std::string_view requested_service_tier,
                                   std::function<void(Request &usage, const GatewayStreamResult &)> on_complete)
{
    res.status = status;
    std::string content_type = "text/event-stream; charset=utf-8";
    for (const UpstreamHeader &header : headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "connection" || lower == "transfer-encoding" || lower == "content-length") {
            continue;
        }
        if (lower == "content-type") {
            content_type = header.value;
            continue;
        }
        res.set_header(header.name, header.value);
    }
    struct Shared {
        UpstreamStreamResponse upstream;
        Request usage;
        std::unique_ptr<Gateway> gateway;
        std::string requested_service_tier;
        int idle_timeout_ms = 0;
        std::function<void(Request &usage, const GatewayStreamResult &)> on_complete;
    };
    auto shared = std::make_shared<Shared>();
    shared->upstream = std::move(upstream);
    shared->usage = std::move(usage);
    shared->gateway = make_gateway_for_usage(shared->usage);
    shared->requested_service_tier = std::string{ requested_service_tier };
    shared->idle_timeout_ms = std::max(1000, config().proxy_upstream_timeout_seconds * 1000);
    shared->on_complete = std::move(on_complete);

    res.set_chunked_content_provider(content_type, [shared](size_t offset, ::httplib::DataSink &sink) mutable {
        if (offset != 0) {
            return false;
        }
        try {
            GatewayStreamResult result;
            auto tracked_write = [&sink](std::string_view data) { return sink.write(data.data(), data.size()); };
            if (shared->gateway) {
                result = pump_gateway_stream(shared->upstream.stream.read, tracked_write, shared->upstream.initial_body,
                                             shared->idle_timeout_ms, shared->upstream.stream.poll_fd,
                                             *shared->gateway);
            } else {
                auto write = [&tracked_write, &result](std::string_view data) {
                    result.pump.response_bytes += data.size();
                    return tracked_write(data);
                };
                if (!shared->upstream.initial_body.empty()) {
                    (void)write(shared->upstream.initial_body);
                }
                char buffer[8192];
                for (;;) {
                    const ssize_t n = shared->upstream.stream.read(buffer, sizeof(buffer));
                    if (n <= 0) {
                        break;
                    }
                    if (!write(std::string_view{ buffer, static_cast<size_t>(n) })) {
                        result.pump.client_disconnected = true;
                        break;
                    }
                }
            }
            if (shared->upstream.stream.close) {
                shared->upstream.stream.close();
            }
            if (shared->on_complete) {
                shared->on_complete(shared->usage, result);
            }
            sink.done();
            return true;
        } catch (const std::exception &err) {
            std::cerr << "chunked stream provider failed: " << err.what() << std::endl;
            try {
                sink.done();
            } catch (...) {
            }
            return false;
        } catch (...) {
            std::cerr << "chunked stream provider failed: unknown" << std::endl;
            try {
                sink.done();
            } catch (...) {
            }
            return false;
        }
    });
}

} // namespace revlm
