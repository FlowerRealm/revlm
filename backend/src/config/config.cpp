#include "config/config.hpp"

#include <cstdlib>
#include <optional>
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

std::optional<Config> g_config;

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
    assign_env(config.site_base_url, "REVLM_SITE_BASE_URL");

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
    config.redis_db = parse_int_config(getenv_trimmed("REVLM_REDIS_DB"), config.redis_db, "REVLM_REDIS_DB");
    config.gateway_retry_base_delay_ms = parse_int_config(getenv_trimmed("REVLM_GATEWAY_RETRY_BASE_DELAY_MS"),
                                                          config.gateway_retry_base_delay_ms,
                                                          "REVLM_GATEWAY_RETRY_BASE_DELAY_MS");
    config.gateway_retry_max_delay_ms = parse_int_config(getenv_trimmed("REVLM_GATEWAY_RETRY_MAX_DELAY_MS"),
                                                         config.gateway_retry_max_delay_ms,
                                                         "REVLM_GATEWAY_RETRY_MAX_DELAY_MS");
    config.routing_rebuild_debounce_ms = parse_int_config(getenv_trimmed("REVLM_ROUTING_REBUILD_DEBOUNCE_MS"),
                                                          config.routing_rebuild_debounce_ms,
                                                          "REVLM_ROUTING_REBUILD_DEBOUNCE_MS");

    validate_config(config);
    return config;
}

void validate_config(Config &cfg)
{
    if (cfg.addr.empty()) {
        throw std::invalid_argument("REVLM_ADDR must not be empty");
    }
    if (cfg.db_dsn.empty()) {
        throw std::invalid_argument("REVLM_DB_DSN must not be empty");
    }
    cfg.site_base_url = normalize_http_base_url(cfg.site_base_url, "REVLM_SITE_BASE_URL");
    if (cfg.shutdown_grace_seconds < 0) {
        throw std::invalid_argument("REVLM_SHUTDOWN_GRACE_PERIOD_SECONDS must not be negative");
    }
    validate_positive(cfg.http_read_header_timeout_seconds, "REVLM_HTTP_READ_HEADER_TIMEOUT_SECONDS");
    validate_positive(cfg.http_max_header_bytes, "REVLM_HTTP_MAX_HEADER_BYTES");
    validate_positive(cfg.http_max_body_bytes, "REVLM_HTTP_MAX_BODY_BYTES");
    validate_positive(cfg.proxy_upstream_timeout_seconds, "REVLM_PROXY_UPSTREAM_TIMEOUT_SECONDS");
    validate_positive(cfg.db_max_open_conns, "REVLM_DB_MAX_OPEN_CONNS");
    validate_positive(cfg.db_max_idle_conns, "REVLM_DB_MAX_IDLE_CONNS");
    if (cfg.db_max_idle_conns > cfg.db_max_open_conns) {
        throw std::invalid_argument("REVLM_DB_MAX_IDLE_CONNS must not exceed REVLM_DB_MAX_OPEN_CONNS");
    }
    validate_non_negative(cfg.redis_db, "REVLM_REDIS_DB");
    if (trim_ascii(cfg.redis_key_prefix).empty()) {
        throw std::invalid_argument("REVLM_REDIS_KEY_PREFIX must not be empty");
    }
    validate_non_negative(cfg.gateway_retry_base_delay_ms, "gateway retry base delay");
    validate_non_negative(cfg.gateway_retry_max_delay_ms, "gateway retry max delay");
    if (cfg.gateway_retry_max_delay_ms < cfg.gateway_retry_base_delay_ms) {
        throw std::invalid_argument("gateway retry max delay must not be less than base delay");
    }
    validate_non_negative(cfg.routing_rebuild_debounce_ms, "routing rebuild debounce");
}

void init_config(Config value)
{
    validate_config(value);
    g_config = std::move(value);
}

const Config &config()
{
    if (!g_config.has_value()) {
        throw std::logic_error("config() called before init_config");
    }
    return *g_config;
}

void reset_config_for_test(Config value)
{
    validate_config(value);
    g_config = std::move(value);
}

} // namespace revlm
