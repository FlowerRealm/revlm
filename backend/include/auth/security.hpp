#pragma once

#include <cstdint>
#include <sys/socket.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace revlm
{

struct TrustedProxy {
    uint32_t network = 0;
    uint32_t mask = 0;
};

struct ValidatedBaseUrl {
    std::string raw;
    std::string scheme;
    std::string host;
    std::string port;
    std::string base_path;
};

std::vector<TrustedProxy> default_trusted_proxies();
bool is_trusted_proxy_ipv4(std::string_view ip, const std::vector<TrustedProxy> &trusted_proxies);
std::optional<std::string> trusted_forwarded_proto(std::string_view raw);
std::optional<std::string> trusted_forwarded_host(std::string_view raw);
std::string redact_request_target(std::string_view target);
ValidatedBaseUrl validate_upstream_base_url(std::string_view raw);
void enforce_upstream_ssrf_guard(const ValidatedBaseUrl &base_url);
bool is_safe_upstream_sockaddr(const sockaddr *addr, socklen_t addr_len);

} // namespace revlm
