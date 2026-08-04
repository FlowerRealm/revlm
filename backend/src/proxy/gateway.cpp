#include "proxy/gateway.hpp"

#include "auth/security.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "config/config.hpp"
#include "models/models.hpp"
#include "proxy/upstream.hpp"
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

// Final usd = plugin-provided runtime base amount * ChannelGroup multiplier
// (ADR-0004). Core never parses token_details or computes billing from it.
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
    const double usd = pr.protocol_cost_usd * pr.upstream.channel_group_multiplier;
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
    req.token_details = pr.token_details;
    req.channel_group_multiplier = pr.upstream.channel_group_multiplier;
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

constexpr size_t kFlushBytes = 1024;
constexpr int kDisconnectDrainTimeoutMs = 1500;

} // namespace

GatewayStreamResult pump_upstream_stream(const std::function<ssize_t(char *, size_t)> &read_chunk,
                                         const std::function<bool(std::string_view)> &write_to_client,
                                         std::string_view initial_body, int idle_timeout_ms, int poll_fd,
                                         const StreamChunkHandler &on_chunk)
{
    GatewayStreamResult out;
    std::string pending_send;
    pending_send.reserve(kFlushBytes);
    const auto started_at = std::chrono::steady_clock::now();

    auto ingest = [&](std::string_view bytes) -> bool {
        out.pump.response_bytes += bytes.size();
        if (out.pump.first_token_latency_ms == 0 && !bytes.empty()) {
            out.pump.first_token_latency_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at)
                    .count());
        }
        if (!out.pump.client_disconnected) {
            pending_send.append(bytes.data(), bytes.size());
            if (pending_send.size() >= kFlushBytes) {
                if (!write_to_client(pending_send)) {
                    out.pump.client_disconnected = true;
                }
                pending_send.clear();
            }
        }
        if (on_chunk) {
            on_chunk(bytes, out.pump);
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
            out.pump.completed = true;
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

    if (!pending_send.empty() && !out.pump.client_disconnected) {
        (void)write_to_client(pending_send);
    }
    return out;
}

// -- v3 plugin data-plane hooks (ADR-0003) ----------------------------------

extern "C" const Channel *revlm_next_candidate(ProxyRequest &proxy)
{
    ChannelGroupSnapshot &group = proxy.channel_group;
    if (group.channels.empty()) {
        return nullptr;
    }
    if (group.pointer < 0 || group.pointer >= static_cast<int>(group.channels.size())) {
        group.pointer = 0;
    }
    bool any_active = false;
    for (const Channel &candidate : group.channels) {
        if (candidate.status) {
            any_active = true;
            break;
        }
    }
    if (!any_active) {
        return nullptr;
    }
    // Round-robin: advance one position and return the next active candidate.
    // The sequence wraps back to the first member indefinitely (ADR-0003); the
    // plugin decides when to stop retrying. nullptr is reserved for an empty or
    // fully-inactive group so the caller can terminate.
    for (;;) {
        group.pointer = (group.pointer + 1) % static_cast<int>(group.channels.size());
        Channel &candidate = group.channels[static_cast<size_t>(group.pointer)];
        if (!candidate.status) {
            continue;
        }
        proxy.upstream.channel_id = candidate.id;
        proxy.upstream.channel_group_multiplier = group.price_multiplier;
        return &candidate;
    }
}

extern "C" bool revlm_commit_request(ProxyRequest &proxy)
{
    // Core applies the ChannelGroup multiplier snapshot; the plugin provides
    // token_details and protocol_cost_usd. The debit + core request record live
    // in commit_proxy_usage.
    if (proxy.channel_group.id > 0 && proxy.channel_group.price_multiplier >= 0.0) {
        proxy.upstream.channel_group_multiplier = proxy.channel_group.price_multiplier;
    }
    return commit_proxy_usage(proxy);
}

// Core default /v1 handler. This is the RTLD_NEXT tail of the plugin chain and
// the no-plugin fallback: with no matching protocol plugin the core returns 500
// without committing or writing a request record (CONTEXT "无匹配协议插件").
extern "C" void revlm_handle_v1(const ::httplib::Request & /* req */, ::httplib::Response &res,
                                ProxyRequest & /* proxy */)
{
    write_proxy_result(
        res, make_proxy_error(500, json{ { "error", json{ { "message", "no matching protocol plugin" } } } }));
}

} // namespace revlm
