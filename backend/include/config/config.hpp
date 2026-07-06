#pragma once

#include <string>
#include <string_view>

namespace revlm
{

enum class RuntimeRole {
    All,
    Web,
    Api,
};

struct Config {
    std::string env = "dev";
    std::string addr = ":8080";
    RuntimeRole role = RuntimeRole::All;
    std::string web_static_dir = "frontend/dist";
    std::string migrations_dir = "internal/store/migrations";
    std::string db_dsn;
    std::string db_migration_lock_name = "revlm.schema_migrations";
    std::string redis_addr;
    std::string redis_password;
    std::string redis_key_prefix = "revlm";
    std::string proxy_upstream_base_url;
    std::string compact_gateway_base_url;
    std::string compact_gateway_key;
    std::string session_secret;
    int shutdown_grace_seconds = 60;
    int http_read_header_timeout_seconds = 5;
    int http_max_header_bytes = 1 << 20;
    int http_max_body_bytes = 4 << 20;
    int proxy_upstream_timeout_seconds = 30;
    int db_max_open_conns = 64;
    int db_max_idle_conns = 32;
    int db_conn_max_lifetime_seconds = 300;
    int db_conn_max_idle_time_seconds = 90;
    int db_migration_lock_timeout_seconds = 30;
    int redis_db = 0;
    int gateway_max_retry_attempts = 2;
    int gateway_retry_base_delay_ms = 300;
    int gateway_retry_max_delay_ms = 1500;
    int gateway_max_retry_elapsed_ms = 10000;
    int gateway_max_failover_switches = 2;
    int gateway_credential_max_concurrency = 64;
    int gateway_wait_timeout_ms = 30000;
    int gateway_wait_queue_extra_slots = 20;
    int routing_refresh_ms = 30000;
    int routing_rebuild_debounce_ms = 500;
    int usage_finalize_flush_ms = 25;
    int usage_finalize_batch_size = 2048;
    int usage_finalize_queue_size = 32768;
    int usage_finalize_workers = 4;
    int usage_commit_poll_ms = 100;
    int usage_commit_claim_size = 1024;
    int usage_commit_workers = 4;
    int usage_commit_lease_ms = 15000;
    int usage_commit_stale_ms = 300000;
};

RuntimeRole parse_runtime_role(std::string value);
std::string runtime_role_name(RuntimeRole role);
bool role_requires_db(RuntimeRole role);
int parse_int_config(const std::string &raw, int fallback, std::string_view key);

Config load_config_from_env();
void validate_config(const Config &config);

} // namespace revlm
