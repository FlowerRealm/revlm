#include "auth/crypto.hpp"
#include "auth/security.hpp"
#include "server/tokens.hpp"
#include "auth/users.hpp"
#include "util/user_input.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace
{

int expect(bool ok, const char *message)
{
    if (ok) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main()
{
    const auto proxies = revlm::default_trusted_proxies();
    if (expect(revlm::is_trusted_proxy_ipv4("127.0.0.1", proxies), "loopback should be trusted") != 0 ||
        expect(revlm::is_trusted_proxy_ipv4("10.1.2.3", proxies), "private proxy range should be trusted") != 0 ||
        expect(!revlm::is_trusted_proxy_ipv4("203.0.113.9", proxies), "public ip should not be trusted") != 0) {
        return 1;
    }

    if (expect(revlm::trusted_forwarded_proto("https, http").value_or("") == "https",
               "forwarded proto should use first token") != 0 ||
        expect(!revlm::trusted_forwarded_proto("javascript").has_value(), "invalid proto should be rejected") != 0 ||
        expect(revlm::trusted_forwarded_host("app.example.test:443").value_or("") == "app.example.test:443",
               "host should allow host:port") != 0 ||
        expect(!revlm::trusted_forwarded_host("evil.example/test").has_value(), "host with path should be rejected") !=
            0) {
        return 1;
    }

    if (expect(revlm::redact_request_target("/api/user/self?token=secret") == "/api/user/self",
               "request target redaction should drop query string") != 0) {
        return 1;
    }

    try {
        const auto valid = revlm::validate_upstream_base_url(" https://api.example.test/v1/ ");
        if (expect(valid.scheme == "https" && valid.host == "api.example.test" && valid.port == "443" &&
                       valid.base_path == "/v1",
                   "validated base URL should normalize scheme host port and path") != 0) {
            return 1;
        }
    } catch (const std::exception &) {
        std::cerr << "valid upstream base URL should pass\n";
        return 1;
    }

    bool bad_scheme_rejected = false;
    try {
        (void)revlm::validate_upstream_base_url("ftp://example.test");
    } catch (const std::invalid_argument &) {
        bad_scheme_rejected = true;
    }
    if (expect(bad_scheme_rejected, "unsupported upstream scheme should be rejected") != 0) {
        return 1;
    }

    bool userinfo_rejected = false;
    try {
        (void)revlm::validate_upstream_base_url("https://user@example.test");
    } catch (const std::invalid_argument &) {
        userinfo_rejected = true;
    }
    if (expect(userinfo_rejected, "upstream base URL userinfo should be rejected") != 0) {
        return 1;
    }

    bool loopback_blocked = false;
    try {
        revlm::enforce_upstream_ssrf_guard(revlm::validate_upstream_base_url("http://127.0.0.1:8080"));
    } catch (const std::invalid_argument &) {
        loopback_blocked = true;
    }
    if (expect(loopback_blocked, "SSRF guard should block loopback upstreams") != 0) {
        return 1;
    }

    bool metadata_blocked = false;
    try {
        revlm::enforce_upstream_ssrf_guard(revlm::validate_upstream_base_url("https://metadata.google.internal"));
    } catch (const std::invalid_argument &) {
        metadata_blocked = true;
    }
    if (expect(metadata_blocked, "SSRF guard should block metadata/internal upstream hosts") != 0) {
        return 1;
    }

    bool ipv4_compatible_loopback_blocked = false;
    try {
        revlm::enforce_upstream_ssrf_guard(revlm::validate_upstream_base_url("http://[::127.0.0.1]:8080"));
    } catch (const std::invalid_argument &) {
        ipv4_compatible_loopback_blocked = true;
    }
    if (expect(ipv4_compatible_loopback_blocked, "SSRF guard should block IPv4-compatible loopback upstreams") != 0) {
        return 1;
    }

    bool six_to_four_blocked = false;
    try {
        revlm::enforce_upstream_ssrf_guard(revlm::validate_upstream_base_url("http://[2002:c000:0204::]:8080"));
    } catch (const std::invalid_argument &) {
        six_to_four_blocked = true;
    }
    if (expect(six_to_four_blocked, "SSRF guard should block 6to4 upstreams") != 0) {
        return 1;
    }

    bool teredo_loopback_blocked = false;
    try {
        revlm::enforce_upstream_ssrf_guard(revlm::validate_upstream_base_url("http://[2001::127.0.0.1]:8080"));
    } catch (const std::invalid_argument &) {
        teredo_loopback_blocked = true;
    }
    if (expect(teredo_loopback_blocked, "SSRF guard should block Teredo embedded loopback upstreams") != 0) {
        return 1;
    }

    bool nat64_loopback_blocked = false;
    try {
        revlm::enforce_upstream_ssrf_guard(revlm::validate_upstream_base_url("http://[64:ff9b::127.0.0.1]:8080"));
    } catch (const std::invalid_argument &) {
        nat64_loopback_blocked = true;
    }
    if (expect(nat64_loopback_blocked, "SSRF guard should block NAT64 embedded loopback upstreams") != 0) {
        return 1;
    }

    try {
        revlm::enforce_compact_gateway_guard(revlm::validate_upstream_base_url("http://127.0.0.1:8080"));
    } catch (const std::exception &) {
        std::cerr << "compact gateway guard should allow loopback for dedicated gateway config\n";
        return 1;
    }

    bool compact_metadata_blocked = false;
    try {
        revlm::enforce_compact_gateway_guard(revlm::validate_upstream_base_url("https://metadata.google.internal"));
    } catch (const std::invalid_argument &) {
        compact_metadata_blocked = true;
    }
    if (expect(compact_metadata_blocked, "compact gateway guard should still block metadata/internal hosts") != 0) {
        return 1;
    }

    const std::string encoded = revlm::base64url_encode("hello");
    const auto decoded = revlm::base64url_decode(encoded);
    if (expect(decoded.has_value() && *decoded == "hello", "base64url should round-trip") != 0 ||
        expect(revlm::sha256_hex("sk_test") == "12b2820cf1639904311da5771de1e5bb65c77073fdc7c555df395942df42896b",
               "sha256 hex should match known digest") != 0 ||
        expect(revlm::constant_time_equal("same", "same") && !revlm::constant_time_equal("same", "diff"),
               "constant time compare should match equal strings only") != 0) {
        return 1;
    }

    const std::string password_hash = revlm::hash_password("password123");
    if (expect(password_hash.rfind("$2b$12$", 0) == 0, "password hash should be bcrypt cost 12") != 0 ||
        expect(revlm::check_password(password_hash, "password123"), "password hash should verify") != 0 ||
        expect(!revlm::check_password(password_hash, "wrong-password"), "wrong password should fail verification") !=
            0) {
        return 1;
    }

    const std::string token_hash = revlm::token_hash("sk_test");
    if (expect(token_hash.size() == 64, "token hash should be hex-encoded SHA-256") != 0 ||
        expect(token_hash == "12b2820cf1639904311da5771de1e5bb65c77073fdc7c555df395942df42896b",
               "token hash should match SHA-256") != 0) {
        return 1;
    }

    return 0;
}
