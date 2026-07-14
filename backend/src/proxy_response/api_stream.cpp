#include "proxy_response/api_stream.hpp"

#include "proxy_response/anthropics_messages.hpp"
#include "proxy_response/openai_chat.hpp"
#include "proxy_response/openai_responses.hpp"
#include "util/json_util.hpp"

#include <poll.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>
#include "util/strings.hpp"

namespace revlm
{
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

const boost::json::object *find_usage_object(const boost::json::value &value)
{
    if (value.is_object()) {
        for (const auto &field : value.as_object()) {
            if (field.key() == "usage" && field.value().is_object()) {
                return &field.value().as_object();
            }
            if (const auto *nested = find_usage_object(field.value())) {
                return nested;
            }
        }
        return nullptr;
    }
    if (value.is_array()) {
        for (const auto &child : value.as_array()) {
            if (const auto *nested = find_usage_object(child)) {
                return nested;
            }
        }
    }
    return nullptr;
}

std::optional<std::string> find_first_model(const boost::json::value &value)
{
    if (value.is_object()) {
        for (const auto &field : value.as_object()) {
            if (field.key() == "model" && field.value().is_string()) {
                const auto model = field.value().as_string();
                if (!model.empty()) {
                    return std::string{ model.data(), model.size() };
                }
            }
            if (const auto nested = find_first_model(field.value())) {
                return nested;
            }
        }
        return std::nullopt;
    }
    if (value.is_array()) {
        for (const auto &child : value.as_array()) {
            if (const auto nested = find_first_model(child)) {
                return nested;
            }
        }
    }
    return std::nullopt;
}

Request &mutable_gateway_request(Gateway &gateway)
{
    struct Access : Gateway {
        static Request &from(Gateway &gw)
        {
            return static_cast<Access &>(gw).request;
        }
    };
    return Access::from(gateway);
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
                      GatewayStreamPump &pump, Gateway &gateway, bool &saw_usage)
{
    if (event.done) {
        pump.completed = true;
        return;
    }
    const auto doc = parse_json(trim_ascii(event.data));
    if (!doc || !doc->is_object()) {
        return;
    }
    if (pump.first_token_latency_ms == 0) {
        pump.first_token_latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at)
                .count());
    }
    boost::json::object json = doc->as_object();
    if (const auto *type = json.if_contains("type"); type != nullptr && type->is_string()) {
        const auto &type_str = type->as_string();
        if (type_str == "message_stop" || type_str == "response.completed") {
            pump.completed = true;
        }
    }
    if (const auto model = find_first_model(json)) {
        pump.model = *model;
    }
    if (find_usage_object(json) != nullptr) {
        gateway.finalize(json);
        saw_usage = true;
    }
}

} // namespace

std::unique_ptr<Gateway> make_gateway(GatewayStreamKind kind, const Model &model, double tier_multiplier,
                                      double channel_multiplier)
{
    switch (kind) {
    case GatewayStreamKind::openai_chat:
        return std::make_unique<OpenaiChatCompletion>(model, tier_multiplier, channel_multiplier);
    case GatewayStreamKind::openai_responses:
        return std::make_unique<OpenaiResponses>(model, tier_multiplier, channel_multiplier);
    case GatewayStreamKind::anthropics_messages:
        return std::make_unique<AnthropicsMessages>(model, tier_multiplier, channel_multiplier);
    }
    return nullptr;
}

Request parse_billing_request_from_body(GatewayStreamKind kind, const Model &model, long long user_id,
                                        std::string_view body, double tier_multiplier, double channel_multiplier)
{
    auto gateway = make_gateway(kind, model, tier_multiplier, channel_multiplier);
    Request billing(model, 0, 0, 0, 0, 0, tier_multiplier, channel_multiplier);
    billing.user_id = user_id;
    if (gateway == nullptr) {
        return billing;
    }
    const auto doc = parse_json(trim_ascii(body));
    if (!doc || !doc->is_object()) {
        return billing;
    }
    boost::json::object json = doc->as_object();
    gateway->finalize(json);
    billing = mutable_gateway_request(*gateway);
    billing.user_id = user_id;
    return billing;
}

GatewayStreamResult pump_gateway_stream(const std::function<ssize_t(char *, size_t)> &read_chunk,
                                        const std::function<bool(std::string_view)> &write_to_client,
                                        std::string_view initial_body, int idle_timeout_ms, int poll_fd,
                                        Gateway &gateway, long long user_id)
{
    GatewayStreamResult out;
    SseReader reader;
    std::vector<SseEvent> events;
    events.reserve(8);
    std::string pending_send;
    pending_send.reserve(kFlushBytes);
    const auto started_at = std::chrono::steady_clock::now();
    bool saw_usage = false;

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
            handle_sse_event(event, started_at, out.pump, gateway, saw_usage);
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

    if (saw_usage) {
        Request billing = mutable_gateway_request(gateway);
        billing.user_id = user_id;
        out.billing_request = std::move(billing);
    }
    return out;
}

void apply_upstream_gateway_stream(::httplib::Response &res, int status, const std::vector<UpstreamHeader> &headers,
                                   UpstreamStreamResponse upstream, const Config &config,
                                   std::unique_ptr<Gateway> gateway, std::string_view requested_service_tier,
                                   long long user_id, std::function<void(const GatewayStreamResult &)> on_complete)
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
        std::unique_ptr<Gateway> gateway;
        std::string requested_service_tier;
        long long user_id = 0;
        int idle_timeout_ms = 0;
        std::function<void(const GatewayStreamResult &)> on_complete;
    };
    auto shared = std::make_shared<Shared>();
    shared->upstream = std::move(upstream);
    shared->gateway = std::move(gateway);
    shared->requested_service_tier = std::string{ requested_service_tier };
    shared->user_id = user_id;
    shared->idle_timeout_ms = std::max(1000, config.proxy_upstream_timeout_seconds * 1000);
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
                                             shared->idle_timeout_ms, shared->upstream.stream.poll_fd, *shared->gateway,
                                             shared->user_id);
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
                shared->on_complete(result);
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
