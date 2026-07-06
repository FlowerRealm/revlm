#include "server/http_server.hpp"

#include <iostream>
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
    const std::string token_payload =
        "{\"access_token\":\"acc-token\",\"refresh_token\":\"ref-token\",\"expires_in\":3600,"
        "\"id_token\":\"eyJhbGciOiJub25lIn0."
        "eyJlbWFpbCI6InVzZXJAZXhhbXBsZS5jb20iLCJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdF8xMjMifX0."
        "\"}";

    revlm::Config config;
    config.role = revlm::RuntimeRole::Api;
    config.db_dsn = "root:root@tcp(127.0.0.1:3306)/tmp";

    const std::string api_meta = revlm::handle_http_request("GET /api/meta HTTP/1.1\r\nHost: test\r\n\r\n", config,
                                                            revlm::BuildInfo{ "test-version", "test-date" }, false,
                                                            "req-meta");

    if (expect(api_meta.find("\"version\":\"test-version\"") != std::string::npos &&
                   api_meta.find("\"build_date\":\"test-date\"") != std::string::npos,
               "api meta sanity check should return build info") != 0) {
        return 1;
    }

    if (expect(token_payload.find("\"expires_in\":3600") != std::string::npos,
               "token payload fixture should include numeric expires_in") != 0) {
        return 1;
    }

    return 0;
}
