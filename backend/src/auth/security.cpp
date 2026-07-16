#include "auth/security.hpp"

#include <arpa/inet.h>

#include <cstring>
#include <cstdint>
#include <netdb.h>
#include <optional>
#include <stdexcept>
#include <string>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

std::string first_forwarded_token(std::string_view raw)
{
    std::string value = trim_ascii(raw);
    const size_t comma = value.find(',');
    if (comma != std::string::npos) {
        value.resize(comma);
    }
    return trim_ascii(value);
}

std::optional<uint32_t> parse_ipv4(std::string_view raw)
{
    in_addr addr{};
    std::string text = trim_ascii(raw);
    if (text.empty() || ::inet_pton(AF_INET, text.c_str(), &addr) != 1) {
        return std::nullopt;
    }
    return ntohl(addr.s_addr);
}

TrustedProxy make_proxy(std::string_view network, int prefix_len)
{
    const auto ip = parse_ipv4(network);
    if (!ip.has_value() || prefix_len < 0 || prefix_len > 32) {
        return {};
    }
    const uint32_t mask = prefix_len == 0 ? 0u : (0xffffffffu << (32 - prefix_len));
    return TrustedProxy{ *ip & mask, mask };
}

bool valid_host_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' ||
           ch == ':' || ch == '[' || ch == ']';
}

std::string default_port_for_scheme(std::string_view scheme)
{
    if (scheme == "http") {
        return "80";
    }
    if (scheme == "https") {
        return "443";
    }
    return {};
}

bool looks_like_ipv6_literal(std::string_view host)
{
    return host.size() >= 2 && host.front() == '[' && host.back() == ']';
}

bool parse_host_port(std::string_view authority, std::string &host, std::string &port)
{
    host.clear();
    port.clear();
    std::string cleaned = trim_ascii(authority);
    if (cleaned.empty()) {
        return false;
    }
    if (looks_like_ipv6_literal(cleaned)) {
        host = cleaned.substr(1, cleaned.size() - 2);
        return !host.empty();
    }
    if (!cleaned.empty() && cleaned.front() == '[') {
        const size_t close = cleaned.find(']');
        if (close == std::string::npos) {
            return false;
        }
        host = cleaned.substr(1, close - 1);
        if (close + 1 < cleaned.size()) {
            if (cleaned[close + 1] != ':') {
                return false;
            }
            port = cleaned.substr(close + 2);
        }
        return !host.empty();
    }
    const size_t colon = cleaned.rfind(':');
    if (colon != std::string::npos && cleaned.find(':') == colon) {
        host = cleaned.substr(0, colon);
        port = cleaned.substr(colon + 1);
    } else {
        host = cleaned;
    }
    return !host.empty();
}

bool is_decimal_port(std::string_view raw)
{
    if (raw.empty()) {
        return false;
    }
    int value = 0;
    for (char ch : raw) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + (ch - '0');
        if (value > 65535) {
            return false;
        }
    }
    return value > 0;
}

bool ipv4_in_cidr(uint32_t ip, uint32_t network, int prefix_len)
{
    if (prefix_len <= 0) {
        return true;
    }
    const uint32_t mask = prefix_len == 32 ? 0xffffffffu : (0xffffffffu << (32 - prefix_len));
    return (ip & mask) == network;
}

bool is_private_or_special_ipv4(uint32_t ip)
{
    return ipv4_in_cidr(ip, 0x00000000u, 8) || // 0.0.0.0/8
           ipv4_in_cidr(ip, 0x0a000000u, 8) || // 10.0.0.0/8
           ipv4_in_cidr(ip, 0x64400000u, 10) || // 100.64.0.0/10
           ipv4_in_cidr(ip, 0x7f000000u, 8) || // 127.0.0.0/8
           ipv4_in_cidr(ip, 0xa9fe0000u, 16) || // 169.254.0.0/16
           ipv4_in_cidr(ip, 0xac100000u, 12) || // 172.16.0.0/12
           ipv4_in_cidr(ip, 0xc0000000u, 24) || // 192.0.0.0/24
           ipv4_in_cidr(ip, 0xc0000200u, 24) || // 192.0.2.0/24
           ipv4_in_cidr(ip, 0xc0a80000u, 16) || // 192.168.0.0/16
           ipv4_in_cidr(ip, 0xc6120000u, 15) || // 198.18.0.0/15
           ipv4_in_cidr(ip, 0xc6336400u, 24) || // 198.51.100.0/24
           ipv4_in_cidr(ip, 0xcb007100u, 24) || // 203.0.113.0/24
           ipv4_in_cidr(ip, 0xe0000000u, 4) || // 224.0.0.0/4
           ipv4_in_cidr(ip, 0xf0000000u, 4); // 240.0.0.0/4
}

bool ipv6_prefix_match(const in6_addr &addr, const uint8_t *prefix, int prefix_bits)
{
    const int whole_bytes = prefix_bits / 8;
    const int remaining_bits = prefix_bits % 8;
    if (whole_bytes > 0 && std::memcmp(addr.s6_addr, prefix, static_cast<size_t>(whole_bytes)) != 0) {
        return false;
    }
    if (remaining_bits == 0) {
        return true;
    }
    const uint8_t mask = static_cast<uint8_t>(0xffu << (8 - remaining_bits));
    return (addr.s6_addr[whole_bytes] & mask) == (prefix[whole_bytes] & mask);
}

void embedded_ipv4_from_ipv6_suffix(const in6_addr &addr, uint32_t &embedded_ipv4)
{
    embedded_ipv4 = (static_cast<uint32_t>(addr.s6_addr[12]) << 24) | (static_cast<uint32_t>(addr.s6_addr[13]) << 16) |
                    (static_cast<uint32_t>(addr.s6_addr[14]) << 8) | static_cast<uint32_t>(addr.s6_addr[15]);
}

bool embedded_ipv4_from_compatible_ipv6(const in6_addr &addr, uint32_t &embedded_ipv4)
{
    static const uint8_t zero_prefix[12] = {};
    if (std::memcmp(addr.s6_addr, zero_prefix, 12) != 0) {
        return false;
    }
    embedded_ipv4_from_ipv6_suffix(addr, embedded_ipv4);
    return true;
}

bool embedded_ipv4_from_teredo_ipv6(const in6_addr &addr, uint32_t &embedded_ipv4)
{
    static const uint8_t teredo_prefix[16] = { 0x20, 0x01 };
    if (!ipv6_prefix_match(addr, teredo_prefix, 32)) {
        return false;
    }
    embedded_ipv4_from_ipv6_suffix(addr, embedded_ipv4);
    return true;
}

bool embedded_ipv4_from_nat64_ipv6(const in6_addr &addr, uint32_t &embedded_ipv4)
{
    static const uint8_t nat64_prefix[16] = { 0, 0x64, 0xff, 0x9b };
    if (!ipv6_prefix_match(addr, nat64_prefix, 96)) {
        return false;
    }
    embedded_ipv4_from_ipv6_suffix(addr, embedded_ipv4);
    return true;
}

bool is_private_or_special_ipv6(const in6_addr &addr)
{
    static const uint8_t loopback[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    static const uint8_t unspecified[16] = {};
    static const uint8_t unique_local[16] = { 0xfc };
    static const uint8_t link_local[16] = { 0xfe, 0x80 };
    static const uint8_t multicast[16] = { 0xff };
    static const uint8_t documentation[16] = { 0x20, 0x01, 0x0d, 0xb8 };
    static const uint8_t six_to_four[16] = { 0x20, 0x02 };
    static const uint8_t ipv4_mapped_prefix[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    uint32_t embedded_ipv4 = 0;
    return std::memcmp(addr.s6_addr, loopback, sizeof(loopback)) == 0 ||
           std::memcmp(addr.s6_addr, unspecified, sizeof(unspecified)) == 0 ||
           ipv6_prefix_match(addr, unique_local, 7) || ipv6_prefix_match(addr, link_local, 10) ||
           ipv6_prefix_match(addr, multicast, 8) || ipv6_prefix_match(addr, documentation, 32) ||
           ipv6_prefix_match(addr, six_to_four, 16) || ipv6_prefix_match(addr, ipv4_mapped_prefix, 96) ||
           (embedded_ipv4_from_compatible_ipv6(addr, embedded_ipv4) && is_private_or_special_ipv4(embedded_ipv4)) ||
           (embedded_ipv4_from_teredo_ipv6(addr, embedded_ipv4) && is_private_or_special_ipv4(embedded_ipv4)) ||
           (embedded_ipv4_from_nat64_ipv6(addr, embedded_ipv4) && is_private_or_special_ipv4(embedded_ipv4));
}

bool hostname_is_denied(std::string_view host)
{
    const std::string lower = lowercase_ascii(trim_ascii(host));
    return lower == "localhost" || lower == "localhost.localdomain" || lower == "metadata.google.internal" ||
           lower == "metadata" || lower.ends_with(".internal") || lower.ends_with(".localhost");
}

bool parse_ipv6_literal(std::string_view raw, in6_addr &out)
{
    std::string text = trim_ascii(raw);
    if (!text.empty() && text.front() == '[' && text.back() == ']') {
        text = text.substr(1, text.size() - 2);
    }
    return !text.empty() && ::inet_pton(AF_INET6, text.c_str(), &out) == 1;
}

} // namespace

std::vector<TrustedProxy> default_trusted_proxies()
{
    return {
        make_proxy("127.0.0.1", 32),
        make_proxy("10.0.0.0", 8),
        make_proxy("172.16.0.0", 12),
        make_proxy("192.168.0.0", 16),
    };
}

bool is_trusted_proxy_ipv4(std::string_view ip, const std::vector<TrustedProxy> &trusted_proxies)
{
    const auto parsed = parse_ipv4(ip);
    if (!parsed.has_value()) {
        return false;
    }
    for (const TrustedProxy &proxy : trusted_proxies) {
        if ((*parsed & proxy.mask) == proxy.network) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> trusted_forwarded_proto(std::string_view raw)
{
    const std::string value = lowercase_ascii(first_forwarded_token(raw));
    if (value == "http" || value == "https") {
        return value;
    }
    return std::nullopt;
}

std::optional<std::string> trusted_forwarded_host(std::string_view raw)
{
    const std::string value = first_forwarded_token(raw);
    if (value.empty()) {
        return std::nullopt;
    }
    for (char ch : value) {
        if (!valid_host_char(ch)) {
            return std::nullopt;
        }
    }
    if (value.find('/') != std::string::npos || value.find('\\') != std::string::npos ||
        value.find('@') != std::string::npos || value.front() == ':' || value.back() == ':') {
        return std::nullopt;
    }
    return value;
}

std::string redact_request_target(std::string_view target)
{
    const size_t query = target.find('?');
    return std::string{ query == std::string_view::npos ? target : target.substr(0, query) };
}

ValidatedBaseUrl validate_upstream_base_url(std::string_view raw)
{
    std::string value = trim_ascii(raw);
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    if (value.empty()) {
        throw std::invalid_argument("upstream base URL must not be empty");
    }
    const size_t scheme_end = value.find("://");
    if (scheme_end == std::string::npos) {
        throw std::invalid_argument("upstream base URL must include scheme");
    }
    const std::string scheme = lowercase_ascii(value.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https") {
        throw std::invalid_argument("upstream base URL only supports http/https");
    }
    const size_t authority_start = scheme_end + 3;
    const size_t authority_end = value.find_first_of("/?#", authority_start);
    if (authority_start >= value.size() || authority_end == authority_start) {
        throw std::invalid_argument("upstream base URL host must not be empty");
    }
    const std::string authority = authority_end == std::string::npos ?
                                      value.substr(authority_start) :
                                      value.substr(authority_start, authority_end - authority_start);
    if (authority.find('@') != std::string::npos) {
        throw std::invalid_argument("upstream base URL must not include userinfo");
    }
    std::string host;
    std::string port;
    if (!parse_host_port(authority, host, port)) {
        throw std::invalid_argument("upstream base URL host must not be empty");
    }
    if (host.empty()) {
        throw std::invalid_argument("upstream base URL host must not be empty");
    }
    for (char ch : host) {
        if (!valid_host_char(ch)) {
            throw std::invalid_argument("upstream base URL host is invalid");
        }
    }
    if (!port.empty() && !is_decimal_port(port)) {
        throw std::invalid_argument("upstream base URL port is invalid");
    }
    ValidatedBaseUrl out;
    out.raw = value;
    out.scheme = scheme;
    out.host = host;
    out.port = port.empty() ? default_port_for_scheme(scheme) : port;
    out.base_path = authority_end == std::string::npos ? std::string{} : value.substr(authority_end);
    if (!out.base_path.empty()) {
        const size_t query = out.base_path.find_first_of("?#");
        if (query != std::string::npos) {
            out.base_path.resize(query);
        }
        while (!out.base_path.empty() && out.base_path.back() == '/') {
            out.base_path.pop_back();
        }
    }
    return out;
}

bool is_safe_upstream_sockaddr(const sockaddr *addr, socklen_t addr_len)
{
    if (addr == nullptr || addr_len <= 0) {
        return false;
    }
    if (addr->sa_family == AF_INET) {
        if (addr_len < static_cast<socklen_t>(sizeof(sockaddr_in))) {
            return false;
        }
        const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(addr);
        return !is_private_or_special_ipv4(ntohl(ipv4->sin_addr.s_addr));
    }
    if (addr->sa_family == AF_INET6) {
        if (addr_len < static_cast<socklen_t>(sizeof(sockaddr_in6))) {
            return false;
        }
        const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(addr);
        return !is_private_or_special_ipv6(ipv6->sin6_addr);
    }
    return false;
}

void enforce_upstream_ssrf_guard(const ValidatedBaseUrl &base_url)
{
    if (base_url.host.empty()) {
        throw std::invalid_argument("upstream base URL host must not be empty");
    }
    if (hostname_is_denied(base_url.host)) {
        throw std::invalid_argument("upstream base URL host is blocked by SSRF guard");
    }

    if (const auto ipv4 = parse_ipv4(base_url.host); ipv4.has_value() && is_private_or_special_ipv4(*ipv4)) {
        throw std::invalid_argument("upstream base URL resolved to blocked address");
    }
    in6_addr ipv6{};
    if (parse_ipv6_literal(base_url.host, ipv6) && is_private_or_special_ipv6(ipv6)) {
        throw std::invalid_argument("upstream base URL resolved to blocked address");
    }
}

} // namespace revlm
