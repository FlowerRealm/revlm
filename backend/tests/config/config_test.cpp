#include "config/config.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{

int expect(bool condition, const char *message)
{
    if (condition) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

template <typename Fn> bool rejects(Fn fn)
{
    try {
        fn();
    } catch (const std::invalid_argument &) {
        return true;
    }
    return false;
}

void unset_test_env()
{
    unsetenv("REVLM_NODE_ROLE");
    unsetenv("REVLM_DB_DSN");
    unsetenv("SESSION_SECRET");
    unsetenv("REVLM_PROXY_UPSTREAM_BASE_URL");
    unsetenv("REVLM_REDIS_DB");
}

} // namespace

int main()
{
    if (expect(revlm::parse_runtime_role("") == revlm::RuntimeRole::All, "empty role should mean all") != 0 ||
        expect(revlm::parse_runtime_role(" API ") == revlm::RuntimeRole::Api, "role should trim and lowercase") != 0 ||
        expect(revlm::parse_runtime_role("all") == revlm::RuntimeRole::All, "all role should parse") != 0 ||
        expect(revlm::parse_runtime_role("web") == revlm::RuntimeRole::Web, "web role should parse") != 0 ||
        expect(revlm::parse_runtime_role("api") == revlm::RuntimeRole::Api, "api role should parse") != 0) {
        return 1;
    }

    if (expect(!revlm::role_requires_db(revlm::RuntimeRole::Web), "web role should not require DB") != 0 ||
        expect(revlm::role_requires_db(revlm::RuntimeRole::All), "all role should require DB") != 0 ||
        expect(revlm::role_requires_db(revlm::RuntimeRole::Api), "api role should require DB") != 0) {
        return 1;
    }

    revlm::Config web;
    web.role = revlm::RuntimeRole::Web;
    web.db_dsn.clear();
    revlm::validate_config(web);

    revlm::Config api;
    api.role = revlm::RuntimeRole::Api;
    bool rejected = false;
    try {
        revlm::validate_config(api);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    if (expect(rejected, "api role without DB should be rejected") != 0) {
        return 1;
    }

    if (expect(revlm::parse_int_config("", 42, "TEST_INT") == 42, "empty int should use fallback") != 0 ||
        expect(revlm::parse_int_config(" 7 ", 42, "TEST_INT") == 7, "valid int should parse") != 0 ||
        expect(rejects([] { (void)revlm::parse_int_config("12x", 42, "TEST_INT"); }),
               "invalid int should be rejected") != 0) {
        return 1;
    }

    revlm::Config tuned;
    tuned.role = revlm::RuntimeRole::Api;
    tuned.db_dsn = "mysql://placeholder";
    tuned.session_secret = "test-secret";
    tuned.db_max_open_conns = 8;
    tuned.db_max_idle_conns = 9;
    if (expect(rejects([&] { revlm::validate_config(tuned); }), "idle DB conns must not exceed open conns") != 0) {
        return 1;
    }

    revlm::Config api_without_secret;
    api_without_secret.role = revlm::RuntimeRole::Api;
    api_without_secret.db_dsn = "mysql://placeholder";
    if (expect(rejects([&] { revlm::validate_config(api_without_secret); }),
               "api role without session secret should be rejected") != 0) {
        return 1;
    }

    unset_test_env();
    setenv("REVLM_NODE_ROLE", "web", 1);
    setenv("REVLM_PROXY_UPSTREAM_BASE_URL", "http://api.example.test///", 1);
    setenv("REVLM_REDIS_DB", "2", 1);
    revlm::Config from_env = revlm::load_config_from_env();
    unset_test_env();
    if (expect(from_env.role == revlm::RuntimeRole::Web, "env role should load") != 0 ||
        expect(from_env.proxy_upstream_base_url == "http://api.example.test",
               "proxy upstream base URL should normalize") != 0 ||
        expect(from_env.redis_db == 2, "redis db should load") != 0) {
        return 1;
    }

    unset_test_env();
    setenv("REVLM_NODE_ROLE", "web", 1);
    setenv("REVLM_PROXY_UPSTREAM_BASE_URL", "http:///api", 1);
    const bool bad_url_rejected = rejects([] { (void)revlm::load_config_from_env(); });
    unset_test_env();
    if (expect(bad_url_rejected, "proxy upstream URL without host should be rejected") != 0) {
        return 1;
    }

    unset_test_env();
    setenv("REVLM_NODE_ROLE", "web", 1);
    setenv("REVLM_PROXY_UPSTREAM_BASE_URL", "https://api.example.test", 1);
    const bool https_proxy_rejected = rejects([] { (void)revlm::load_config_from_env(); });
    unset_test_env();
    if (expect(https_proxy_rejected, "web edge proxy should reject unsupported https upstream") != 0) {
        return 1;
    }

    return 0;
}
