#include "proxy/transport.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <vector>

#include "auth/security.hpp"
#include "users/users.hpp"
#include "util/strings.hpp"

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

UpstreamRequest build_proxy_upstream_request(const ProxyRequest &pr, std::string_view path)
{
    const std::string &client_ip = pr.client_ip;

    auto header_string = [&pr](std::string_view wanted_lower) -> std::string {
        for (const auto &kv : pr.headers) {
            if (lowercase_ascii(kv.first) == wanted_lower) {
                return kv.second;
            }
        }
        return {};
    };

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
    headers.push_back({ "X-Request-Id", pr.request_id });
    headers.push_back({ "X-Forwarded-Proto", forwarded_proto });
    if (!original_host.empty()) {
        headers.push_back({ "X-Forwarded-Host", original_host });
    }
    if (!client_ip.empty()) {
        headers.push_back({ "X-Forwarded-For", client_ip });
    }
    for (const auto &kv : pr.headers) {
        const std::string lower = lowercase_ascii(kv.first);
        if (is_hop_by_hop_header(kv.first) || lower == "host" || lower == "connection" || lower == "content-length" ||
            lower == "x-request-id" || lower == "x-forwarded-for" || lower == "x-forwarded-host" ||
            lower == "x-forwarded-proto") {
            continue;
        }
        // The client's own credentials must never reach an upstream: the channel
        // supplies the upstream key, and forwarding the caller's would leak it.
        if (lower == "authorization" || lower == "x-api-key") {
            std::fprintf(stderr, "WARNING: build_proxy_upstream_request found sensitive header '%.*s' - stripping\n",
                         static_cast<int>(kv.first.size()), kv.first.data());
            continue;
        }
        headers.push_back({ kv.first, kv.second });
    }

    UpstreamRequest downstream;
    downstream.method = pr.method.empty() ? std::string{ "POST" } : pr.method;
    downstream.path = std::string{ path };
    downstream.body = pr.body;
    downstream.headers = std::move(headers);
    return downstream;
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

void set_stream_correlation_headers(::httplib::Response &res, std::string_view response_id)
{
    if (!response_id.empty()) {
        res.set_header("X-Response-Id", std::string{ response_id });
    }
}

std::vector<UpstreamHeader> merge_correlation_headers(const std::vector<UpstreamHeader> &upstream_headers,
                                                      std::string_view response_id)
{
    std::vector<UpstreamHeader> headers;
    headers.reserve(upstream_headers.size() + 1);
    for (const UpstreamHeader &header : upstream_headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "x-request-id" || lower == "x-response-id") {
            continue;
        }
        headers.push_back({ header.name, header.value });
    }
    if (!response_id.empty()) {
        headers.push_back({ "X-Response-Id", std::string{ response_id } });
    }
    return headers;
}

std::optional<json> paygo_balance_gate(long long user_id)
{
    if (UserStore::instance().has_positive_user_balance(user_id)) {
        return std::nullopt;
    }
    return json({ { "error", json({ { "message", "insufficient balance" } }) } });
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

} // namespace revlm
