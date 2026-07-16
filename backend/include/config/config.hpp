#pragma once

#include <string>
#include <string_view>

namespace revlm
{

struct Config {
    std::string env = "dev";
    std::string addr = ":8080";
    std::string db_dsn;
    std::string redis_addr;
    std::string redis_password;
    std::string redis_key_prefix = "revlm";
    std::string session_secret;
    int shutdown_grace_seconds = 60;
    int http_read_header_timeout_seconds = 5;
    int http_max_header_bytes = 1 << 20;
    int http_max_body_bytes = 4 << 20;
    int proxy_upstream_timeout_seconds = 30;
    int db_max_open_conns = 64;
    int db_max_idle_conns = 32;
    int redis_db = 0;
    int gateway_max_retry_attempts = 2;
    int gateway_retry_base_delay_ms = 300;
    int gateway_retry_max_delay_ms = 1500;
    int gateway_max_retry_elapsed_ms = 10000;
    int gateway_max_failover_switches = 2;
    int routing_rebuild_debounce_ms = 500;
};

int parse_int_config(const std::string &raw, int fallback, std::string_view key);

Config load_config_from_env();
void validate_config(const Config &cfg);

void init_config(Config value);
const Config &config();
void reset_config_for_test(Config value);

} // namespace revlm
