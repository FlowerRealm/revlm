#include "config/config.hpp"
#include "server/http_server.hpp"

#include <iostream>
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
    revlm::reset_config_for_test(config);

    const std::string ready = revlm::handle_http_request("GET /readyz HTTP/1.1\r\nHost: test\r\n\r\n", false, "1001");
    if (expect_contains(ready, "HTTP/1.1 200 OK", "readyz should be ready before drain") != 0 ||
        expect_contains(ready, "X-Request-Id: 1001", "response should include request id") != 0) {
        return 1;
    }

    const std::string missing_request_id =
        revlm::handle_http_request("GET /readyz HTTP/1.1\r\nHost: test\r\n\r\n", false, "");
    if (expect_contains(missing_request_id, "HTTP/1.1 400 Bad Request", "missing X-Request-Id should be rejected") !=
            0 ||
        expect_contains(missing_request_id, "missing X-Request-Id",
                        "missing X-Request-Id should explain the failure") != 0) {
        return 1;
    }

    const std::string client_request_id = revlm::handle_http_request(
        "GET /readyz HTTP/1.1\r\nHost: test\r\nx-client-request-id: client-uuid-1\r\n\r\n", false, "");
    if (expect_contains(client_request_id, "HTTP/1.1 200 OK", "x-client-request-id should be accepted as request id") !=
            0 ||
        expect_contains(client_request_id, "X-Request-Id: client-uuid-1", "response should echo x-client-request-id") !=
            0) {
        return 1;
    }

    const std::string draining = revlm::handle_http_request("GET /readyz HTTP/1.1\r\nHost: test\r\n\r\n", true, "1002");
    if (expect_contains(draining, "HTTP/1.1 503 Service Unavailable", "readyz should fail while draining") != 0 ||
        expect_contains(draining, "draining", "drain response should explain state") != 0) {
        return 1;
    }

    config.http_max_body_bytes = 3;
    revlm::reset_config_for_test(config);
    const std::string too_large = revlm::handle_http_request(
        "POST /api/user/login HTTP/1.1\r\nHost: test\r\nContent-Length: 4\r\n\r\nxxxx", false, "1003");
    if (expect_contains(too_large, "HTTP/1.1 413 Payload Too Large", "large body should be rejected") != 0) {
        return 1;
    }

    config.http_max_header_bytes = 8;
    revlm::reset_config_for_test(config);
    const std::string header_large =
        revlm::handle_http_request("GET /readyz HTTP/1.1\r\nHost: test\r\n\r\n", false, "1004");
    if (expect_contains(header_large, "HTTP/1.1 431 Request Header Fields Too Large",
                        "large header should be rejected") != 0) {
        return 1;
    }

    config.http_max_header_bytes = 1 << 20;
    config.http_max_body_bytes = 4 << 20;
    revlm::reset_config_for_test(config);

    const std::string spa =
        revlm::handle_http_request("GET /admin/users HTTP/1.1\r\nHost: test\r\n\r\n", false, "1005");
    if (expect_contains(spa, "HTTP/1.1 404 Not Found", "unknown paths should not be served") != 0) {
        return 1;
    }

    const std::string admin_dashboard_query =
        revlm::handle_http_request("GET /api/admin/dashboard?from=test HTTP/1.1\r\nHost: test\r\n\r\n", false, "1006");
    if (expect_contains(admin_dashboard_query, "HTTP/1.1 200 OK", "api routes should ignore query for routing") != 0 ||
        expect_contains(admin_dashboard_query, "\"success\":false",
                        "admin dashboard with query should still return api json") != 0) {
        return 1;
    }

    const std::string admin_dashboard_requires_auth =
        revlm::handle_http_request("GET /api/admin/dashboard HTTP/1.1\r\nHost: test\r\n\r\n", false, "1007");
    if (expect_contains(admin_dashboard_requires_auth, "HTTP/1.1 200 OK",
                        "admin dashboard route should exist and return api json") != 0 ||
        expect_contains(admin_dashboard_requires_auth, "\"success\":false",
                        "admin dashboard route should report auth failure without session") != 0 ||
        expect_contains(admin_dashboard_requires_auth, "未登录",
                        "admin dashboard route should require authentication") != 0) {
        return 1;
    }

    const std::string channel_page_unauth =
        revlm::handle_http_request("GET /api/channel/page HTTP/1.1\r\nHost: test\r\n\r\n", false, "1008");
    if (expect_contains(channel_page_unauth, "\"success\":false", "channel page should require admin auth") != 0 ||
        expect_contains(channel_page_unauth, "未登录", "channel page should reject unauthenticated callers") != 0) {
        return 1;
    }

    const std::string bad_usage_tz = revlm::handle_http_request(
        "GET /api/request/windows?tz=../../bad HTTP/1.1\r\nHost: test\r\n\r\n", false, "1009");
    if (expect_contains(bad_usage_tz, "\"success\":false", "usage windows route should be reachable") != 0) {
        return 1;
    }

    const std::string bad_usage_date = revlm::handle_http_request(
        "GET /api/request/timeseries?start=2026-02-31 HTTP/1.1\r\nHost: test\r\n\r\n", false, "1010");
    if (expect_contains(bad_usage_date, "\"success\":false", "usage timeseries route should be reachable") != 0) {
        return 1;
    }

    const std::string bad_usage_granularity = revlm::handle_http_request(
        "GET /api/request/timeseries?granularity=month HTTP/1.1\r\nHost: test\r\n\r\n", false, "1011");
    if (expect_contains(bad_usage_granularity, "\"success\":false",
                        "usage timeseries route should be reachable for invalid granularity probe") != 0) {
        return 1;
    }

    return 0;
}
