#include "config/config.hpp"
#include "util/user_input.hpp"

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
    unsetenv("REVLM_DB_DSN");
    unsetenv("SESSION_SECRET");
    unsetenv("REVLM_REDIS_DB");
    unsetenv("REVLM_SITE_BASE_URL");
    unsetenv("REVLM_BILLING_PAYGO_PRICE_MULTIPLIER");
}

} // namespace

int main()
{
    revlm::Config api;
    api.db_dsn = "mysql://placeholder";
    api.session_secret = "test-secret";
    revlm::validate_config(api);

    revlm::Config missing_db;
    missing_db.session_secret = "test-secret";
    if (expect(rejects([&] { revlm::validate_config(missing_db); }), "missing DB DSN should be rejected") != 0) {
        return 1;
    }

    if (expect(revlm::parse_int_config("", 42, "TEST_INT") == 42, "empty int should use fallback") != 0 ||
        expect(revlm::parse_int_config(" 7 ", 42, "TEST_INT") == 7, "valid int should parse") != 0 ||
        expect(rejects([] { (void)revlm::parse_int_config("12x", 42, "TEST_INT"); }),
               "invalid int should be rejected") != 0) {
        return 1;
    }

    revlm::Config tuned;
    tuned.db_dsn = "mysql://placeholder";
    tuned.session_secret = "test-secret";
    tuned.db_max_open_conns = 8;
    tuned.db_max_idle_conns = 9;
    if (expect(rejects([&] { revlm::validate_config(tuned); }), "idle DB conns must not exceed open conns") != 0) {
        return 1;
    }

    revlm::Config api_without_secret;
    api_without_secret.db_dsn = "mysql://placeholder";
    if (expect(rejects([&] { revlm::validate_config(api_without_secret); }),
               "missing session secret should be rejected") != 0) {
        return 1;
    }

    revlm::Config with_site;
    with_site.db_dsn = "mysql://placeholder";
    with_site.session_secret = "test-secret";
    with_site.site_base_url = " https://example.com/root/ ";
    revlm::validate_config(with_site);
    if (expect(with_site.site_base_url == "https://example.com/root",
               "site_base_url should trim and drop trailing slash") != 0) {
        return 1;
    }

    revlm::Config bad_site = with_site;
    bad_site.site_base_url = "example.com";
    if (expect(rejects([&] { revlm::validate_config(bad_site); }), "site_base_url must require http/https scheme") !=
        0) {
        return 1;
    }

    revlm::Config bad_paygo = with_site;
    bad_paygo.billing_paygo_price_multiplier = 0.0;
    if (expect(rejects([&] { revlm::validate_config(bad_paygo); }),
               "billing_paygo_price_multiplier must be positive") != 0) {
        return 1;
    }

    if (expect(revlm::normalize_http_base_url(" https://example.com/ ", "site_base_url") == "https://example.com",
               "normalize_http_base_url should trim and drop trailing slash") != 0 ||
        expect(revlm::normalize_price_multiplier_value("01.25") == "1.250000",
               "normalize_price_multiplier_value should normalize decimals") != 0 ||
        expect(rejects([] { (void)revlm::normalize_http_base_url("example.com", "site_base_url"); }),
               "normalize_http_base_url must require scheme") != 0 ||
        expect(rejects([] { (void)revlm::normalize_price_multiplier_value("0"); }),
               "normalize_price_multiplier_value must reject non-positive") != 0) {
        return 1;
    }

    unset_test_env();
    setenv("REVLM_DB_DSN", "mysql://placeholder", 1);
    setenv("SESSION_SECRET", "test-secret", 1);
    setenv("REVLM_REDIS_DB", "2", 1);
    setenv("REVLM_SITE_BASE_URL", "https://admin.example.com/base/", 1);
    setenv("REVLM_BILLING_PAYGO_PRICE_MULTIPLIER", "1.5", 1);
    revlm::Config from_env = revlm::load_config_from_env();
    unset_test_env();
    if (expect(from_env.db_dsn == "mysql://placeholder", "DB DSN should load from env") != 0 ||
        expect(from_env.session_secret == "test-secret", "session secret should load from env") != 0 ||
        expect(from_env.redis_db == 2, "redis db should load") != 0 ||
        expect(from_env.site_base_url == "https://admin.example.com/base", "site_base_url should load from env") != 0 ||
        expect(from_env.billing_paygo_price_multiplier == 1.5, "paygo multiplier should load from env") != 0) {
        return 1;
    }

    return 0;
}
