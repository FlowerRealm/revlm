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
    unsetenv("REVLM_DB_DSN");
    unsetenv("SESSION_SECRET");
    unsetenv("REVLM_REDIS_DB");
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

    unset_test_env();
    setenv("REVLM_DB_DSN", "mysql://placeholder", 1);
    setenv("SESSION_SECRET", "test-secret", 1);
    setenv("REVLM_REDIS_DB", "2", 1);
    revlm::Config from_env = revlm::load_config_from_env();
    unset_test_env();
    if (expect(from_env.db_dsn == "mysql://placeholder", "DB DSN should load from env") != 0 ||
        expect(from_env.session_secret == "test-secret", "session secret should load from env") != 0 ||
        expect(from_env.redis_db == 2, "redis db should load") != 0) {
        return 1;
    }

    return 0;
}
