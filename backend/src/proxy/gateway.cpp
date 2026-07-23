#include "proxy/gateway.hpp"

#include "auth/security.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "config/config.hpp"
#include "models/models.hpp"
#include "proxy/upstream.hpp"
#include "proxy/anthropics_messages.hpp"
#include "proxy/openai_chat.hpp"
#include "proxy/openai_responses.hpp"
#include "request/request.hpp"
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

void write_upstream(::httplib::Response &res, int status, std::string body, const std::vector<UpstreamHeader> &headers)
{
    res.status = status;
    res.reason = (status >= 200 && status < 300) ? "OK" : "Upstream";
    std::string content_type = "application/json; charset=utf-8";
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
    res.set_content(std::move(body), content_type);
}

json headers_to_json(const std::vector<UpstreamHeader> &headers)
{
    json out;
    for (const UpstreamHeader &header : headers) {
        out[header.name] = header.value;
    }
    return out;
}

std::vector<UpstreamHeader> headers_from_json(const json &header_obj)
{
    std::vector<UpstreamHeader> out;
    if (!header_obj.is_object()) {
        return out;
    }
    for (const auto &key : header_obj.keys()) {
        const auto value = header_obj[key].as_string();
        if (!value.has_value()) {
            continue;
        }
        out.push_back({ key, *value });
    }
    return out;
}

json make_proxy_result(int status, std::string body, const std::vector<UpstreamHeader> &headers)
{
    return json({ { "status", status }, { "header", headers_to_json(headers) }, { "body", std::move(body) } });
}

json make_proxy_error(int status, json error_body)
{
    return make_proxy_result(status, serialize(error_body), { { "Content-Type", "application/json; charset=utf-8" } });
}

void write_proxy_result(::httplib::Response &res, const json &result)
{
    const int status = static_cast<int>(result["status"].as_int64().value_or(500));
    const std::string body = result["body"].as_string().value_or("");
    write_upstream(res, status, body, headers_from_json(result["header"]));
}

std::string upstream_response_id_from_headers(const std::vector<UpstreamHeader> &headers)
{
    std::string fallback;
    for (const UpstreamHeader &header : headers) {
        const std::string lower = lowercase_ascii(header.name);
        const std::string value = trim_ascii(header.value);
        if (value.empty())
            continue;
        if (lower == "x-request-id")
            return value;
        if (fallback.empty() && lower == "request-id")
            fallback = value;
    }
    return fallback;
}

void assign_request_correlation(ProxyRequest &pr, std::string_view response_id)
{
    if (!response_id.empty())
        pr.upstream.response_id = std::string{ response_id };
}

void set_stream_correlation_headers(::httplib::Response &res, std::string_view response_id)
{
    if (!response_id.empty())
        res.set_header("X-Response-Id", std::string{ response_id });
}

std::vector<UpstreamHeader> merge_correlation_headers(const std::vector<UpstreamHeader> &upstream_headers,
                                                      std::string_view response_id)
{
    std::vector<UpstreamHeader> headers;
    headers.reserve(upstream_headers.size() + 2);
    for (const UpstreamHeader &header : upstream_headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "x-request-id" || lower == "x-response-id")
            continue;
        headers.push_back({ header.name, header.value });
    }
    if (!response_id.empty())
        headers.push_back({ "X-Response-Id", std::string{ response_id } });
    return headers;
}

std::optional<json> paygo_balance_gate(long long user_id)
{
    if (UserStore::instance().has_positive_user_balance(user_id))
        return std::nullopt;
    return json({ { "error", json({ { "message", "insufficient balance" } }) } });
}

bool commit_proxy_usage(ProxyRequest &pr)
{
    if (pr.id <= 0)
        return false;
    if (pr.auth.user_id <= 0)
        return false;
    if (pr.auth.token_id <= 0)
        return false;
    if (pr.upstream.channel_id <= 0)
        return false;
    const double usd = compute_usd(pr);
    if (!UserStore::instance().debit_user_balance_usd(pr.auth.user_id, usd))
        return false;
    Request req;
    req.id = pr.id;
    req.time = pr.time;
    req.date = pr.time.substr(0, 10);
    req.user_id = pr.auth.user_id;
    req.request_id = pr.request_id;
    req.response_id = pr.upstream.response_id;
    req.endpoint = pr.http.path;
    req.method = pr.http.method;
    req.token_id = pr.auth.token_id;
    req.input_tokens = pr.usage.input_tokens;
    req.output_tokens = pr.usage.output_tokens;
    req.cache_read_tokens = pr.usage.cache_read_tokens;
    req.cache_creation_1h_tokens = pr.usage.cache_creation_1h_tokens;
    req.cache_creation_5m_tokens = pr.usage.cache_creation_5m_tokens;
    req.tier_multiplier = pr.upstream.tier_multiplier;
    req.service_tier = pr.upstream.service_tier;
    req.channel_multiplier = pr.upstream.channel_multiplier;
    req.channel_id = pr.upstream.channel_id;
    req.status_code = pr.upstream.status_code;
    req.latency_ms = pr.upstream.latency_ms;
    req.first_token_latency_ms = pr.upstream.first_token_latency_ms;
    req.is_stream = pr.is_stream;
    req.model_name = pr.upstream.model_name;
    if (!pr.error_class.empty())
        req.error_class = pr.error_class;
    if (!pr.error_message.empty())
        req.error_message = pr.error_message;
    req.usd = usd;
    return req.commit(pr.time);
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
        if (!channel.has_value())
            throw std::runtime_error("channel not found");
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
    if (!value || !value->is_object())
        return std::string{ json_text };
    value->erase(field_name);
    return value->dump();
}

UpstreamRequest build_proxy_upstream_request(const ProxyRequest &pr, std::string_view path)
{
    const std::string &client_ip = pr.http.client_ip;

    auto header_string = [&pr](std::string_view wanted_lower) -> std::string {
        for (const auto &kv : pr.http.headers) {
            if (lowercase_ascii(kv.first) == wanted_lower) {
                return kv.second;
            }
        }
        return {};
    };

    const std::string request_id = pr.request_id;

    std::string original_host = header_string("host");
    std::string forwarded_proto = "http";
    if (is_trusted_proxy_ipv4(client_ip, default_trusted_proxies())) {
        if (const auto host = trusted_forwarded_host(header_string("x-forwarded-host")); host.has_value()) {
            original_host = *host;
        }
        if (const auto proto = trusted_forwarded_proto(header_string("x-forwarded-proto")); proto.has_value()) {
            forwarded_proto = *proto;
        }
    }

    std::vector<UpstreamHeader> headers;
    headers.push_back({ "X-Request-Id", request_id });
    headers.push_back({ "X-Forwarded-Proto", forwarded_proto });
    if (!original_host.empty()) {
        headers.push_back({ "X-Forwarded-Host", original_host });
    }
    if (!client_ip.empty()) {
        headers.push_back({ "X-Forwarded-For", client_ip });
    }
    for (const auto &kv : pr.http.headers) {
        const std::string lower = lowercase_ascii(kv.first);
        if (is_hop_by_hop_header(kv.first) || lower == "host" || lower == "connection" || lower == "content-length" ||
            lower == "x-request-id" || lower == "x-forwarded-for" || lower == "x-forwarded-host" ||
            lower == "x-forwarded-proto") {
            continue;
        }
        if (lower == "authorization" || lower == "x-api-key") {
            std::fprintf(stderr, "WARNING: build_proxy_upstream_request found sensitive header '%.*s' - stripping\n",
                         static_cast<int>(kv.first.size()), kv.first.data());
            continue;
        }
        headers.push_back({ kv.first, kv.second });
    }

    UpstreamRequest downstream;
    downstream.method = "POST";
    downstream.path = std::string{ path };
    downstream.body = pr.http.body;
    downstream.headers = std::move(headers);
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
                                                 const std::vector<UpstreamHeader> &headers)
{
    std::ostringstream out;
    out << "HTTP/1.1 " << status << (status >= 200 && status < 300 ? " OK" : " Bad Gateway") << "\r\n"
        << "Content-Type: " << (content_type.empty() ? "text/event-stream; charset=utf-8" : content_type) << "\r\n";
    for (const UpstreamHeader &header : headers) {
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
        if (value["usage"].is_object()) {
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
            if (key == "model") {
                if (const auto model = value[key].as_string(); model.has_value() && !model->empty()) {
                    return *model;
                }
            }
            if (const auto nested = find_first_model(value[key])) {
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
    auto doc = json::parse(trim_ascii(event.data));
    if (!doc || !doc->is_object()) {
        return;
    }
    if (pump.first_token_latency_ms == 0) {
        pump.first_token_latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at)
                .count());
    }
    const json &root = *doc;
    if (const auto type_str = root["type"].as_string(); type_str.has_value()) {
        if (*type_str == "message_stop" || *type_str == "response.completed") {
            pump.completed = true;
        }
    }
    if (const auto model = find_first_model(root)) {
        pump.model = *model;
    }
    if (contains_usage_object(root)) {
        gateway.finalize(*doc);
        pump.saw_usage = true;
    }
}

} // namespace

UpstreamRequest Gateway::make_upstream(bool stream) const
{
    (void)stream;
    return build_proxy_upstream_request(request, upstream_path());
}

void Gateway::fill_success_pricing(ProxyRequest &pr, const Channel &channel)
{
    pr.upstream.channel_multiplier = channel.price_multiplier;
    if (const Model *model = channel.find_model(pr.upstream.model_name)) {
        fill_pricing_from_model(pr.upstream.pricing, *model);
    }
}

bool Gateway::should_bill_non_stream() const
{
    return true;
}

bool Gateway::prepare(::httplib::Response &res)
{
    (void)res;
    return true;
}

std::string_view Gateway::no_available_channel_message() const
{
    return "no available channel";
}

std::optional<ChannelGroup> Gateway::load_channel_group() const
{
    const long long channel_group_id = request.auth.channel_group_id;
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

json Gateway::run()
{
    auto group = load_channel_group();
    if (!group.has_value()) {
        return make_proxy_error(400, json{ { "error", json{ { "message", "channel group unavailable" } } } });
    }

    const int start = group->pointer;
    int last_status = 502;
    std::string last_body = serialize(json{ { "error", json{ { "message", "proxy upstream failed" } } } });
    std::vector<UpstreamHeader> last_headers;
    bool tried = false;

    do {
        Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
        if (channel_ok(channel)) {
            tried = true;
            ScheduledUpstreamExecution executed = execute_scheduled_upstream(channel.id, make_upstream(false));
            if (executed.result.has_value() && executed.result->response.status_code < 400) {
                UpstreamResponse &resp = executed.result->response;
                const std::string response_id = upstream_response_id_from_headers(resp.headers);
                request.upstream.channel_id = channel.id;
                request.upstream.model_name = parse_json_string_field(resp.body, "model").value_or("");
                fill_success_pricing(request, channel);
                request.upstream.status_code = resp.status_code;
                request.upstream.response_id = response_id;
                assign_request_correlation(request, response_id);
                if (const auto response_tier = parse_json_string_field(resp.body, "service_tier");
                    response_tier.has_value()) {
                    request.upstream.service_tier = *response_tier;
                }
                parse_billing_request_from_body(request, kind(), resp.body);
                request.http.body.clear();
                request.http.body.shrink_to_fit();
                return make_proxy_result(resp.status_code, std::move(resp.body),
                                         merge_correlation_headers(resp.headers, response_id));
            }
            if (executed.result.has_value()) {
                UpstreamResponse &resp = executed.result->response;
                const std::string response_id = upstream_response_id_from_headers(resp.headers);
                last_status = resp.status_code;
                last_body = std::move(resp.body);
                last_headers = merge_correlation_headers(resp.headers, response_id);
                request.upstream.channel_id = channel.id;
                request.upstream.status_code = resp.status_code;
            } else {
                last_status = 502;
                last_body = serialize(json{ { "error", json{ { "message", "proxy upstream failed" } } } });
                last_headers = {};
                request.upstream.channel_id = channel.id;
                request.upstream.status_code = 502;
            }
        }
        group->next_channel();
    } while (group->pointer != start);

    if (!tried) {
        return make_proxy_error(
            400, json{ { "error", json{ { "message", std::string{ no_available_channel_message() } } } } });
    }
    return make_proxy_result(last_status, std::move(last_body), last_headers);
}

void Gateway::run_stream(::httplib::Response &res, const std::function<void(ProxyRequest &)> &on_usage)
{
    auto group = load_channel_group();
    if (!group.has_value()) {
        write_proxy_result(
            res, make_proxy_error(400, json{ { "error", json{ { "message", "channel group unavailable" } } } }));
        return;
    }

    const int start = group->pointer;
    int last_status = 502;
    std::string last_body = serialize(json{ { "error", json{ { "message", "proxy upstream failed" } } } });
    std::vector<UpstreamHeader> last_headers;
    bool tried = false;

    do {
        Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
        if (channel_ok(channel)) {
            tried = true;
            ScheduledUpstreamStreamExecution executed = open_scheduled_upstream_stream(channel.id, make_upstream(true));
            if (executed.result.has_value() && executed.result->status_code < 400) {
                UpstreamStreamResponse upstream = std::move(*executed.result);
                const int status = upstream.status_code;
                const std::string response_id = upstream_response_id_from_headers(upstream.headers);
                const long long channel_id = channel.id;
                const double route_mult = channel.price_multiplier;
                request.upstream.channel_id = channel_id;
                request.upstream.status_code = status;
                request.upstream.channel_multiplier = route_mult;
                request.is_stream = true;
                request.upstream.response_id = response_id;
                assign_request_correlation(request, response_id);

                request.http.body.clear();
                request.http.body.shrink_to_fit();
                const GatewayStreamKind stream_kind = kind();
                apply_upstream_gateway_stream(
                    res, status, upstream.headers, std::move(upstream), std::move(request),
                    [stream_kind](ProxyRequest &u) -> std::unique_ptr<Gateway> { return make_gateway(stream_kind, u); },
                    [status, on_usage, channel_id, route_mult](ProxyRequest &u, const GatewayStreamResult &result) {
                        const GatewayStreamPump &pump = result.pump;
                        const bool success = status < 400 && pump.completed && !pump.upstream_error &&
                                             !pump.idle_timeout;
                        if (!on_usage || !success || !pump.saw_usage) {
                            return;
                        }
                        if (const auto channel = ChannelStore::instance().find_channel(channel_id);
                            channel.has_value()) {
                            u.upstream.channel_multiplier = route_mult;
                            if (const Model *model = channel->find_model(u.upstream.model_name)) {
                                fill_pricing_from_model(u.upstream.pricing, *model);
                            }
                        }
                        u.upstream.first_token_latency_ms = pump.first_token_latency_ms;
                        on_usage(u);
                    });
                set_stream_correlation_headers(res, response_id);
                return;
            }
            if (!executed.result.has_value()) {
                last_status = 502;
                last_body = serialize(json{ { "error", json{ { "message", "proxy upstream failed" } } } });
                last_headers = {};
                request.upstream.channel_id = channel.id;
                request.upstream.status_code = 502;
            } else {
                const int status = executed.result->status_code;
                last_status = status;
                last_body = drain_upstream_stream_body(*executed.result);
                last_headers = merge_correlation_headers(executed.result->headers, {});
                request.upstream.channel_id = channel.id;
                request.upstream.status_code = status;
            }
        }
        group->next_channel();
    } while (group->pointer != start);

    if (!tried) {
        write_proxy_result(
            res, make_proxy_error(
                     400, json{ { "error", json{ { "message", std::string{ no_available_channel_message() } } } } }));
        return;
    }
    write_upstream(res, last_status, std::move(last_body), last_headers);
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

json gateway_json_error_body(std::string_view message)
{
    return json{ { "error", json{ { "message", std::string{ message } } } } };
}

void write_proxy_upstream_response(::httplib::Response &res, const ProxyUpstreamResponse &upstream)
{
    std::vector<UpstreamHeader> headers;
    if (!upstream.response_id.empty()) {
        headers.push_back({ "X-Response-Id", upstream.response_id });
    }
    if (!upstream.content_type.empty()) {
        headers.push_back({ "Content-Type", upstream.content_type });
    }
    write_upstream(res, upstream.status, upstream.body, headers);
}

std::string content_type_from_headers(const std::vector<UpstreamHeader> &headers)
{
    for (const auto &header : headers) {
        if (lowercase_ascii(header.name) == "content-type") {
            return trim_ascii(header.value);
        }
    }
    return {};
}

ProxyUpstreamResponse perform_gateway_upstream_request(long long channel_id, UpstreamRequest downstream)
{
    const ScheduledUpstreamExecution executed = execute_scheduled_upstream(channel_id, std::move(downstream));
    if (!executed.result.has_value()) {
        throw std::runtime_error(executed.transport_error->message.empty() ? "upstream unavailable" :
                                                                             executed.transport_error->message);
    }
    return ProxyUpstreamResponse{
        .status = executed.result->response.status_code,
        .body = std::move(executed.result->response.body),
        .content_type = content_type_from_headers(executed.result->response.headers),
        .response_id = upstream_response_id_from_headers(executed.result->response.headers),
    };
}

UpstreamSession open_gateway_upstream_stream_session(long long channel_id, UpstreamRequest downstream)
{
    ScheduledUpstreamStreamExecution executed = open_scheduled_upstream_stream(channel_id, std::move(downstream));
    if (!executed.result.has_value()) {
        throw std::runtime_error(executed.transport_error->message.empty() ? "upstream unavailable" :
                                                                             executed.transport_error->message);
    }
    UpstreamStreamResponse stream_response = std::move(*executed.result);
    UpstreamSession session;
    session.head.status = stream_response.status_code;
    session.head.body = std::move(stream_response.initial_body);
    session.head.content_type = content_type_from_headers(stream_response.headers);
    session.head.response_id = upstream_response_id_from_headers(stream_response.headers);
    session.stream = std::move(stream_response.stream);
    return session;
}

void stream_gateway_session_to_httplib(::httplib::Response &res, UpstreamSession session, ProxyRequest usage,
                                       GatewayStreamKind stream_kind, double route_group_multiplier,
                                       std::function<void(ProxyRequest &usage, int first_token_latency_ms)> on_complete)
{
    const int stream_status = session.head.status;
    const std::string content_type = session.head.content_type.empty() ? "text/event-stream; charset=utf-8" :
                                                                         session.head.content_type;
    res.status = stream_status;
    res.set_header("Content-Type", content_type);
    if (!session.head.response_id.empty()) {
        res.set_header("X-Response-Id", session.head.response_id);
    }

    struct Shared {
        UpstreamSession session;
        ProxyRequest usage;
        GatewayStreamKind stream_kind = GatewayStreamKind::openai_responses;
        double channel_multiplier = 1.0;
        std::function<void(ProxyRequest &, int)> on_complete;
    };
    auto shared = std::make_shared<Shared>();
    shared->session = std::move(session);
    shared->usage = std::move(usage);
    shared->usage.upstream.channel_multiplier = route_group_multiplier;
    shared->stream_kind = stream_kind;
    shared->channel_multiplier = route_group_multiplier;
    shared->on_complete = std::move(on_complete);

    const int idle_timeout_ms = std::max(1000, config().proxy_upstream_timeout_seconds * 1000);
    res.set_chunked_content_provider(content_type, [shared, idle_timeout_ms](size_t offset,
                                                                             ::httplib::DataSink &sink) mutable {
        if (offset != 0) {
            return false;
        }
        auto stream_gateway = make_gateway(shared->stream_kind, shared->usage);
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

bool stream_gateway_session_to_client(UpstreamSession &session, const ClientWriter &write_client, ProxyRequest &usage,
                                      GatewayStreamKind stream_kind,
                                      std::optional<std::string> &upstream_response_model, long long &response_bytes,
                                      int &first_token_latency_ms, bool &had_usage_out)
{
    std::vector<UpstreamHeader> headers;
    if (!session.head.response_id.empty()) {
        headers.push_back({ "X-Response-Id", session.head.response_id });
    }
    if (!write_client(build_synthetic_stream_response_head(session.head.status, session.head.content_type, headers))) {
        return false;
    }
    auto gateway = make_gateway(stream_kind, usage);
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

Gateway::HandleResult Gateway::handle(::httplib::Response &res)
{
    return handle(res, StreamOptions{});
}

Gateway::HandleResult Gateway::handle(::httplib::Response &res, const StreamOptions &options)
{
    if (!prepare(res)) {
        return {};
    }

    const bool stream_requested = request.is_stream;
    const auto request_started_at = Clock::now();
    const auto elapsed_latency_ms = [&request_started_at]() {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - request_started_at).count());
    };

    try {
        // Copy before clear so upstream still receives the original body.
        const std::string body = request.http.body;
        request.http.body.clear();
        request.http.body.shrink_to_fit();

        auto group = load_channel_group();
        if (!group.has_value()) {
            write_upstream(res, 400, serialize(gateway_json_error_body("channel group unavailable")),
                           { { "Content-Type", "application/json; charset=utf-8" } });
            return {};
        }

        ClientWriter write_client = options.write_client;
        if (!write_client && options.client_fd >= 0) {
            write_client = client_writer_from_fd(options.client_fd);
        }
        const bool stream_sink_ready = write_client || options.stream_response != nullptr;

        const int start = group->pointer;
        int last_status = 502;
        std::string last_body = serialize(gateway_json_error_body("proxy upstream failed"));
        std::vector<UpstreamHeader> last_headers;
        bool tried = false;

        do {
            Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
            if (!channel_ok(channel)) {
                group->next_channel();
                continue;
            }
            tried = true;
            const long long channel_id = channel.id;
            const double route_mult = channel.price_multiplier;

            // Temporarily restore body for make_upstream (chat-style builders read request.http.body).
            request.http.body = body;
            UpstreamRequest downstream = make_upstream(stream_requested && stream_sink_ready);
            request.http.body.clear();
            request.http.body.shrink_to_fit();

            std::optional<UpstreamSession> stream_session;
            ProxyUpstreamResponse upstream;
            bool attempt_failed = false;

            try {
                if (stream_requested && stream_sink_ready) {
                    UpstreamSession session = open_gateway_upstream_stream_session(channel_id, std::move(downstream));
                    if (session.head.status >= 400) {
                        upstream = ProxyUpstreamResponse{
                            .status = session.head.status,
                            .body = session.head.body + read_remaining_stream(session.stream),
                            .content_type = session.head.content_type,
                            .response_id = session.head.response_id,
                        };
                        last_status = upstream.status;
                        last_body = upstream.body;
                        if (!upstream.response_id.empty()) {
                            last_headers.push_back({ "X-Response-Id", upstream.response_id });
                        }
                        if (!upstream.content_type.empty()) {
                            last_headers.push_back({ "Content-Type", upstream.content_type });
                        }
                        request.upstream.channel_id = channel_id;
                        request.upstream.status_code = upstream.status;
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
                    upstream = perform_gateway_upstream_request(channel_id, std::move(downstream));
                    if (upstream.status >= 400) {
                        last_status = upstream.status;
                        last_body = upstream.body;
                        last_headers = {};
                        if (!upstream.response_id.empty()) {
                            last_headers.push_back({ "X-Response-Id", upstream.response_id });
                        }
                        if (!upstream.content_type.empty()) {
                            last_headers.push_back({ "Content-Type", upstream.content_type });
                        }
                        request.upstream.channel_id = channel_id;
                        request.upstream.status_code = upstream.status;
                        attempt_failed = true;
                    }
                }
            } catch (const std::exception &err) {
                last_status = 502;
                last_body = serialize(gateway_json_error_body(err.what()));
                last_headers = {};
                request.upstream.channel_id = channel_id;
                request.upstream.status_code = 502;
                attempt_failed = true;
            }

            if (attempt_failed) {
                group->next_channel();
                continue;
            }

            if (stream_session.has_value()) {
                UpstreamSession &session = *stream_session;
                const int stream_status = session.head.status;
                const GatewayStreamKind stream_kind = kind();
                request.upstream.channel_id = channel_id;
                request.upstream.status_code = stream_status;
                request.upstream.channel_multiplier = route_mult;
                request.is_stream = true;
                request.upstream.response_id = session.head.response_id;

                auto finish_stream_billing = [&](ProxyRequest &stream_request, int first_token_latency_ms) {
                    fill_success_pricing(stream_request, channel);
                    stream_request.upstream.latency_ms = elapsed_latency_ms();
                    stream_request.upstream.first_token_latency_ms =
                        std::min(std::max(first_token_latency_ms, 0), std::max(stream_request.upstream.latency_ms, 0));
                    stream_request.upstream.channel_multiplier = route_mult;
                };
                if (options.stream_response != nullptr) {
                    ProxyRequest stream_usage = request;
                    stream_gateway_session_to_httplib(*options.stream_response, std::move(session),
                                                      std::move(stream_usage), stream_kind, route_mult,
                                                      [&](ProxyRequest &stream_request, int first_token_latency_ms) {
                                                          if (stream_status >= 400 || !options.on_usage) {
                                                              return;
                                                          }
                                                          finish_stream_billing(stream_request, first_token_latency_ms);
                                                          options.on_usage(stream_request);
                                                      });
                    return HandleResult{
                        .handled_stream = true,
                        .stream_status = stream_status,
                    };
                }
                std::optional<std::string> upstream_response_model;
                long long response_bytes = 0;
                int first_token_latency_ms = 0;
                bool had_usage = false;
                if (!stream_gateway_session_to_client(session, write_client, request, stream_kind,
                                                      upstream_response_model, response_bytes, first_token_latency_ms,
                                                      had_usage)) {
                    throw std::runtime_error("stream pump failed");
                }
                if (session.head.status < 400 && had_usage) {
                    finish_stream_billing(request, first_token_latency_ms);
                    (void)commit_proxy_usage(request);
                }
                return HandleResult{
                    .handled_stream = true,
                    .stream_status = session.head.status,
                };
            }

            if (!should_bill_non_stream()) {
                write_proxy_upstream_response(res, upstream);
                return {};
            }

            if (upstream.status >= 400) {
                write_proxy_upstream_response(res, upstream);
                return {};
            }

            request.upstream.channel_id = channel_id;
            request.upstream.status_code = upstream.status;
            request.is_stream = false;
            request.upstream.latency_ms = std::max(elapsed_latency_ms(), 0);
            request.upstream.response_id = upstream.response_id;
            request.upstream.tier_multiplier = 1.0;
            parse_billing_request_from_body(request, kind(), upstream.body);
            fill_success_pricing(request, channel);
            write_proxy_upstream_response(res, upstream);
            return {};
        } while (group->pointer != start);

        if (!tried) {
            write_upstream(res, 400, serialize(gateway_json_error_body(no_available_channel_message())),
                           { { "Content-Type", "application/json; charset=utf-8" } });
            return {};
        }
        write_upstream(res, last_status, std::move(last_body), last_headers);
        return {};
    } catch (const std::exception &err) {
        write_upstream(res, 502, serialize(gateway_json_error_body(err.what())),
                       { { "Content-Type", "application/json; charset=utf-8" } });
        return {};
    }
}

std::unique_ptr<Gateway> make_gateway(GatewayStreamKind kind, ProxyRequest &pr)
{
    switch (kind) {
    case GatewayStreamKind::openai_chat:
        return std::make_unique<OpenaiChatCompletion>(pr);
    case GatewayStreamKind::openai_responses:
        return std::make_unique<OpenaiResponses>(pr);
    case GatewayStreamKind::anthropics_messages:
        return std::make_unique<AnthropicsMessages>(pr);
    }
    return nullptr;
}

void parse_billing_request_from_body(ProxyRequest &pr, GatewayStreamKind kind, std::string_view body)
{
    auto gateway = make_gateway(kind, pr);
    if (gateway == nullptr) {
        return;
    }
    auto doc = json::parse(trim_ascii(body));
    if (!doc || !doc->is_object()) {
        return;
    }
    gateway->finalize(*doc);
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
                                   UpstreamStreamResponse upstream, ProxyRequest usage,
                                   std::function<std::unique_ptr<Gateway>(ProxyRequest &)> make_gateway_for_usage,
                                   std::function<void(ProxyRequest &usage, const GatewayStreamResult &)> on_complete)
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
        ProxyRequest usage;
        std::unique_ptr<Gateway> gateway;
        int idle_timeout_ms = 0;
        std::function<void(ProxyRequest &usage, const GatewayStreamResult &)> on_complete;
    };
    auto shared = std::make_shared<Shared>();
    shared->upstream = std::move(upstream);
    shared->usage = std::move(usage);
    shared->gateway = make_gateway_for_usage(shared->usage);
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
