#include "config/config.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include "util/strings.hpp"
#include "util/user_input.hpp"

namespace revlm
{
namespace
{

std::string getenv_or_empty(const char *key)
{
    const char *value = std::getenv(key);
    return value == nullptr ? std::string{} : std::string{ value };
}

std::string getenv_trimmed(const char *key)
{
    return trim_ascii(getenv_or_empty(key));
}

void assign_env(std::string &target, const char *key)
{
    if (std::string value = getenv_trimmed(key); !value.empty()) {
        target = value;
    }
}

void validate_non_negative(int value, std::string_view key)
{
    if (value < 0) {
        throw std::invalid_argument(std::string{ key } + " must not be negative");
    }
}

void validate_positive(int value, std::string_view key)
{
    if (value <= 0) {
        throw std::invalid_argument(std::string{ key } + " must be positive");
    }
}

} // namespace

int parse_int_config(const std::string &raw, int fallback, std::string_view key)
{
    const std::string value_text = trim_ascii(raw);
    if (value_text.empty()) {
        return fallback;
    }
    try {
        size_t pos = 0;
        int value = std::stoi(value_text, &pos, 10);
        if (pos != value_text.size()) {
            throw std::invalid_argument(std::string{ key } + " must be an integer");
        }
        return value;
    } catch (const std::invalid_argument &) {
        throw std::invalid_argument(std::string{ key } + " must be an integer");
    } catch (const std::out_of_range &) {
        throw std::invalid_argument(std::string{ key } + " is out of range");
    }
}

Config load_config_from_env()
{
    Config config;

    assign_env(config.env, "REVLM_ENV");
    assign_env(config.addr, "REVLM_ADDR");
    assign_env(config.db_dsn, "REVLM_DB_DSN");
    assign_env(config.redis_addr, "REVLM_REDIS_ADDR");
    assign_env(config.redis_password, "REVLM_REDIS_PASSWORD");
    assign_env(config.redis_key_prefix, "REVLM_REDIS_KEY_PREFIX");
    assign_env(config.compact_gateway_base_url, "REVLM_COMPACT_GATEWAY_BASE_URL");
    assign_env(config.compact_gateway_key, "REVLM_COMPACT_GATEWAY_KEY");
    assign_env(config.session_secret, "SESSION_SECRET");

    config.shutdown_grace_seconds = parse_int_config(getenv_trimmed("REVLM_SHUTDOWN_GRACE_PERIOD_SECONDS"),
                                                     config.shutdown_grace_seconds,
                                                     "REVLM_SHUTDOWN_GRACE_PERIOD_SECONDS");
    config.db_max_open_conns = parse_int_config(getenv_trimmed("REVLM_DB_MAX_OPEN_CONNS"), config.db_max_open_conns,
                                                "REVLM_DB_MAX_OPEN_CONNS");
    config.db_max_idle_conns = parse_int_config(getenv_trimmed("REVLM_DB_MAX_IDLE_CONNS"), config.db_max_idle_conns,
                                                "REVLM_DB_MAX_IDLE_CONNS");
    config.http_read_header_timeout_seconds = parse_int_config(getenv_trimmed("REVLM_HTTP_READ_HEADER_TIMEOUT_SECONDS"),
                                                               config.http_read_header_timeout_seconds,
                                                               "REVLM_HTTP_READ_HEADER_TIMEOUT_SECONDS");
    config.http_max_header_bytes = parse_int_config(getenv_trimmed("REVLM_HTTP_MAX_HEADER_BYTES"),
                                                    config.http_max_header_bytes, "REVLM_HTTP_MAX_HEADER_BYTES");
    config.http_max_body_bytes = parse_int_config(getenv_trimmed("REVLM_HTTP_MAX_BODY_BYTES"),
                                                  config.http_max_body_bytes, "REVLM_HTTP_MAX_BODY_BYTES");
    config.proxy_upstream_timeout_seconds = parse_int_config(getenv_trimmed("REVLM_PROXY_UPSTREAM_TIMEOUT_SECONDS"),
                                                             config.proxy_upstream_timeout_seconds,
                                                             "REVLM_PROXY_UPSTREAM_TIMEOUT_SECONDS");
    config.db_conn_max_lifetime_seconds = parse_int_config(getenv_trimmed("REVLM_DB_CONN_MAX_LIFETIME_SECONDS"),
                                                           config.db_conn_max_lifetime_seconds,
                                                           "REVLM_DB_CONN_MAX_LIFETIME_SECONDS");
    config.db_conn_max_idle_time_seconds = parse_int_config(getenv_trimmed("REVLM_DB_CONN_MAX_IDLE_TIME_SECONDS"),
                                                            config.db_conn_max_idle_time_seconds,
                                                            "REVLM_DB_CONN_MAX_IDLE_TIME_SECONDS");
    config.redis_db = parse_int_config(getenv_trimmed("REVLM_REDIS_DB"), config.redis_db, "REVLM_REDIS_DB");
    config.gateway_max_retry_attempts = parse_int_config(getenv_trimmed("REVLM_GATEWAY_MAX_RETRY_ATTEMPTS"),
                                                         config.gateway_max_retry_attempts,
                                                         "REVLM_GATEWAY_MAX_RETRY_ATTEMPTS");
    config.gateway_retry_base_delay_ms = parse_int_config(getenv_trimmed("REVLM_GATEWAY_RETRY_BASE_DELAY_MS"),
                                                          config.gateway_retry_base_delay_ms,
                                                          "REVLM_GATEWAY_RETRY_BASE_DELAY_MS");
    config.gateway_retry_max_delay_ms = parse_int_config(getenv_trimmed("REVLM_GATEWAY_RETRY_MAX_DELAY_MS"),
                                                         config.gateway_retry_max_delay_ms,
                                                         "REVLM_GATEWAY_RETRY_MAX_DELAY_MS");
    config.gateway_max_retry_elapsed_ms = parse_int_config(getenv_trimmed("REVLM_GATEWAY_MAX_RETRY_ELAPSED_MS"),
                                                           config.gateway_max_retry_elapsed_ms,
                                                           "REVLM_GATEWAY_MAX_RETRY_ELAPSED_MS");
    config.gateway_max_failover_switches = parse_int_config(getenv_trimmed("REVLM_GATEWAY_MAX_FAILOVER_SWITCHES"),
                                                            config.gateway_max_failover_switches,
                                                            "REVLM_GATEWAY_MAX_FAILOVER_SWITCHES");
    config.gateway_credential_max_concurrency =
        parse_int_config(getenv_trimmed("REVLM_GATEWAY_CREDENTIAL_MAX_CONCURRENCY"),
                         config.gateway_credential_max_concurrency, "REVLM_GATEWAY_CREDENTIAL_MAX_CONCURRENCY");
    config.gateway_wait_timeout_ms = parse_int_config(getenv_trimmed("REVLM_GATEWAY_WAIT_TIMEOUT_MS"),
                                                      config.gateway_wait_timeout_ms, "REVLM_GATEWAY_WAIT_TIMEOUT_MS");
    config.gateway_wait_queue_extra_slots = parse_int_config(getenv_trimmed("REVLM_GATEWAY_WAIT_QUEUE_EXTRA_SLOTS"),
                                                             config.gateway_wait_queue_extra_slots,
                                                             "REVLM_GATEWAY_WAIT_QUEUE_EXTRA_SLOTS");
    config.routing_refresh_ms = parse_int_config(getenv_trimmed("REVLM_ROUTING_REFRESH_MS"), config.routing_refresh_ms,
                                                 "REVLM_ROUTING_REFRESH_MS");
    config.routing_rebuild_debounce_ms = parse_int_config(getenv_trimmed("REVLM_ROUTING_REBUILD_DEBOUNCE_MS"),
                                                          config.routing_rebuild_debounce_ms,
                                                          "REVLM_ROUTING_REBUILD_DEBOUNCE_MS");
    config.usage_finalize_flush_ms = parse_int_config(getenv_trimmed("REVLM_USAGE_FINALIZE_FLUSH_MS"),
                                                      config.usage_finalize_flush_ms, "REVLM_USAGE_FINALIZE_FLUSH_MS");
    config.usage_finalize_batch_size = parse_int_config(getenv_trimmed("REVLM_USAGE_FINALIZE_BATCH_SIZE"),
                                                        config.usage_finalize_batch_size,
                                                        "REVLM_USAGE_FINALIZE_BATCH_SIZE");
    config.usage_finalize_queue_size = parse_int_config(getenv_trimmed("REVLM_USAGE_FINALIZE_QUEUE_SIZE"),
                                                        config.usage_finalize_queue_size,
                                                        "REVLM_USAGE_FINALIZE_QUEUE_SIZE");
    config.usage_finalize_workers = parse_int_config(getenv_trimmed("REVLM_USAGE_FINALIZE_WORKERS"),
                                                     config.usage_finalize_workers, "REVLM_USAGE_FINALIZE_WORKERS");
    config.usage_commit_poll_ms = parse_int_config(getenv_trimmed("REVLM_USAGE_COMMIT_POLL_MS"),
                                                   config.usage_commit_poll_ms, "REVLM_USAGE_COMMIT_POLL_MS");
    config.usage_commit_claim_size = parse_int_config(getenv_trimmed("REVLM_USAGE_COMMIT_CLAIM_SIZE"),
                                                      config.usage_commit_claim_size, "REVLM_USAGE_COMMIT_CLAIM_SIZE");
    config.usage_commit_workers = parse_int_config(getenv_trimmed("REVLM_USAGE_COMMIT_WORKERS"),
                                                   config.usage_commit_workers, "REVLM_USAGE_COMMIT_WORKERS");
    config.usage_commit_lease_ms = parse_int_config(getenv_trimmed("REVLM_USAGE_COMMIT_LEASE_MS"),
                                                    config.usage_commit_lease_ms, "REVLM_USAGE_COMMIT_LEASE_MS");
    config.usage_commit_stale_ms = parse_int_config(getenv_trimmed("REVLM_USAGE_COMMIT_STALE_MS"),
                                                    config.usage_commit_stale_ms, "REVLM_USAGE_COMMIT_STALE_MS");

    config.compact_gateway_base_url =
        normalize_http_base_url(config.compact_gateway_base_url, "REVLM_COMPACT_GATEWAY_BASE_URL");

    validate_config(config);
    return config;
}

void validate_config(const Config &config)
{
    if (config.addr.empty()) {
        throw std::invalid_argument("REVLM_ADDR must not be empty");
    }
    if (config.db_dsn.empty()) {
        throw std::invalid_argument("REVLM_DB_DSN must not be empty");
    }
    if (trim_ascii(config.session_secret).empty()) {
        throw std::invalid_argument("SESSION_SECRET must not be empty");
    }
    if (config.shutdown_grace_seconds < 0) {
        throw std::invalid_argument("REVLM_SHUTDOWN_GRACE_PERIOD_SECONDS must not be negative");
    }
    if (trim_ascii(config.migrations_dir).empty()) {
        throw std::invalid_argument("migrations dir must not be empty");
    }
    if (trim_ascii(config.db_migration_lock_name).empty()) {
        throw std::invalid_argument("migration lock name must not be empty");
    }
    validate_positive(config.http_read_header_timeout_seconds, "REVLM_HTTP_READ_HEADER_TIMEOUT_SECONDS");
    validate_positive(config.http_max_header_bytes, "REVLM_HTTP_MAX_HEADER_BYTES");
    validate_positive(config.http_max_body_bytes, "REVLM_HTTP_MAX_BODY_BYTES");
    validate_positive(config.proxy_upstream_timeout_seconds, "REVLM_PROXY_UPSTREAM_TIMEOUT_SECONDS");
    validate_positive(config.db_max_open_conns, "REVLM_DB_MAX_OPEN_CONNS");
    validate_positive(config.db_max_idle_conns, "REVLM_DB_MAX_IDLE_CONNS");
    if (config.db_max_idle_conns > config.db_max_open_conns) {
        throw std::invalid_argument("REVLM_DB_MAX_IDLE_CONNS must not exceed REVLM_DB_MAX_OPEN_CONNS");
    }
    validate_non_negative(config.db_conn_max_lifetime_seconds, "REVLM_DB_CONN_MAX_LIFETIME_SECONDS");
    validate_non_negative(config.db_conn_max_idle_time_seconds, "REVLM_DB_CONN_MAX_IDLE_TIME_SECONDS");
    validate_non_negative(config.db_migration_lock_timeout_seconds, "migration lock timeout");
    validate_non_negative(config.redis_db, "REVLM_REDIS_DB");
    if (trim_ascii(config.redis_key_prefix).empty()) {
        throw std::invalid_argument("REVLM_REDIS_KEY_PREFIX must not be empty");
    }
    (void)normalize_http_base_url(config.compact_gateway_base_url, "REVLM_COMPACT_GATEWAY_BASE_URL");
    validate_non_negative(config.gateway_max_retry_attempts, "gateway max retry attempts");
    validate_non_negative(config.gateway_retry_base_delay_ms, "gateway retry base delay");
    validate_non_negative(config.gateway_retry_max_delay_ms, "gateway retry max delay");
    if (config.gateway_retry_max_delay_ms < config.gateway_retry_base_delay_ms) {
        throw std::invalid_argument("gateway retry max delay must not be less than base delay");
    }
    validate_non_negative(config.gateway_max_retry_elapsed_ms, "gateway retry elapsed");
    validate_non_negative(config.gateway_max_failover_switches, "gateway failover switches");
    validate_positive(config.gateway_credential_max_concurrency, "gateway credential concurrency");
    validate_positive(config.gateway_wait_timeout_ms, "gateway wait timeout");
    validate_non_negative(config.gateway_wait_queue_extra_slots, "gateway wait queue extra slots");
    validate_positive(config.routing_refresh_ms, "routing refresh");
    validate_non_negative(config.routing_rebuild_debounce_ms, "routing rebuild debounce");
    validate_positive(config.usage_finalize_flush_ms, "usage finalize flush");
    validate_positive(config.usage_finalize_batch_size, "usage finalize batch size");
    validate_positive(config.usage_finalize_queue_size, "usage finalize queue size");
    validate_positive(config.usage_finalize_workers, "usage finalize workers");
    validate_positive(config.usage_commit_poll_ms, "usage commit poll");
    validate_positive(config.usage_commit_claim_size, "usage commit claim size");
    validate_positive(config.usage_commit_workers, "usage commit workers");
    validate_positive(config.usage_commit_lease_ms, "usage commit lease");
    validate_positive(config.usage_commit_stale_ms, "usage commit stale");
}

} // namespace revlm
