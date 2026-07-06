#include "runtime/runtime_workers.hpp"
#include "server/http_server.hpp"

#include <atomic>
#include <iostream>
#include <memory>
#include <string>

namespace
{

int expect_contains(const std::string &haystack, const char *needle, const char *message)
{
    if (haystack.find(needle) != std::string::npos) {
        return 0;
    }
    std::cerr << message << '\n' << haystack << '\n';
    return 1;
}

} // namespace

int main()
{
    revlm::Config config;
    config.db_dsn = "mysql://placeholder";
    config.session_secret = "test-secret";
    revlm::BuildInfo build{ "test-version", "test-date" };

    const std::string ready =
        revlm::handle_http_request("GET /readyz HTTP/1.1\r\nHost: test\r\n\r\n", config, build, false, "req-test");
    if (expect_contains(ready, "HTTP/1.1 200 OK", "readyz should be ready before drain") != 0 ||
        expect_contains(ready, "X-Request-Id: req-test", "response should include request id") != 0) {
        return 1;
    }

    const std::string draining =
        revlm::handle_http_request("GET /readyz HTTP/1.1\r\nHost: test\r\n\r\n", config, build, true, "req-drain");
    if (expect_contains(draining, "HTTP/1.1 503 Service Unavailable", "readyz should fail while draining") != 0 ||
        expect_contains(draining, "draining", "drain response should explain state") != 0) {
        return 1;
    }

    config.http_max_body_bytes = 3;
    const std::string too_large = revlm::handle_http_request(
        "POST /api/meta HTTP/1.1\r\nHost: test\r\nContent-Length: 4\r\n\r\nxxxx", config, build, false, "req-large");
    if (expect_contains(too_large, "HTTP/1.1 413 Payload Too Large", "large body should be rejected") != 0) {
        return 1;
    }

    config.http_max_header_bytes = 8;
    const std::string header_large =
        revlm::handle_http_request("GET /healthz HTTP/1.1\r\nHost: test\r\n\r\n", config, build, false, "req-header");
    if (expect_contains(header_large, "HTTP/1.1 431 Request Header Fields Too Large",
                        "large header should be rejected") != 0) {
        return 1;
    }

    config.http_max_header_bytes = 1 << 20;
    config.http_max_body_bytes = 4 << 20;

    const std::string spa = revlm::handle_http_request("GET /admin/users HTTP/1.1\r\nHost: test\r\n\r\n", config,
                                                       build, false, "req-spa");
    if (expect_contains(spa, "HTTP/1.1 404 Not Found", "unknown paths should not be served") != 0) {
        return 1;
    }

    const std::string api_meta_query = revlm::handle_http_request(
        "GET /api/meta?from=test HTTP/1.1\r\nHost: test\r\n\r\n", config, build, false, "req-api-meta");
    if (expect_contains(api_meta_query, "HTTP/1.1 200 OK", "api meta should ignore query for routing") != 0 ||
        expect_contains(api_meta_query, "\"version\":\"test-version\"", "api meta should still return build info") !=
            0) {
        return 1;
    }

    const std::string admin_settings_requires_auth = revlm::handle_http_request(
        "GET /api/admin/settings HTTP/1.1\r\nHost: test\r\n\r\n", config, build, false, "req-admin-settings");
    if (expect_contains(admin_settings_requires_auth, "HTTP/1.1 200 OK",
                        "admin settings route should exist and return api json") != 0 ||
        expect_contains(admin_settings_requires_auth, "\"success\":false",
                        "admin settings route should report auth failure without session") != 0 ||
        expect_contains(admin_settings_requires_auth, "未登录", "admin settings route should require authentication") !=
            0) {
        return 1;
    }

    const std::string channel_page_unauth = revlm::handle_http_request(
        "GET /api/channel/page HTTP/1.1\r\nHost: test\r\n\r\n", config, build, false, "req-channel-page");
    if (expect_contains(channel_page_unauth, "\"success\":false", "channel page should require admin auth") != 0 ||
        expect_contains(channel_page_unauth, "未登录", "channel page should reject unauthenticated callers") != 0) {
        return 1;
    }

    auto usage_finalize_queue_depth = std::make_shared<std::atomic_ullong>(12);
    revlm::RuntimeWorkerRegistry registry;
    registry.usage_finalize_queue_depth = usage_finalize_queue_depth;
    revlm::install_runtime_worker_registry(std::move(registry));
    const std::string metrics = revlm::handle_http_request("GET /metrics HTTP/1.1\r\nHost: test\r\n\r\n", config,
                                                           build, false, "req-metrics");
    revlm::clear_runtime_worker_registry();
    if (expect_contains(metrics, "HTTP/1.1 200 OK", "metrics should be served") != 0 ||
        expect_contains(metrics, "revlm_usage_finalize_queue_depth 12", "metrics should expose finalize queue depth") !=
            0) {
        return 1;
    }

    const std::string bad_usage_tz =
        revlm::handle_http_request("GET /api/usage/windows?tz=../../bad HTTP/1.1\r\nHost: test\r\n\r\n", config, build,
                                   false, "req-bad-usage-tz");
    if (expect_contains(bad_usage_tz, "\"success\":false", "usage windows route should be reachable") != 0) {
        return 1;
    }

    const std::string bad_usage_date =
        revlm::handle_http_request("GET /api/usage/timeseries?start=2026-02-31 HTTP/1.1\r\nHost: test\r\n\r\n", config,
                                   build, false, "req-bad-usage-date");
    if (expect_contains(bad_usage_date, "\"success\":false", "usage timeseries route should be reachable") != 0) {
        return 1;
    }

    const std::string bad_usage_granularity =
        revlm::handle_http_request("GET /api/usage/timeseries?granularity=month HTTP/1.1\r\nHost: test\r\n\r\n", config,
                                   build, false, "req-bad-usage-granularity");
    if (expect_contains(bad_usage_granularity, "\"success\":false",
                        "usage timeseries route should be reachable for invalid granularity probe") != 0) {
        return 1;
    }

    return 0;
}
