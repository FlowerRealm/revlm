-- Drop legacy usage rollup / job tables (no-op if already absent).
DROP TABLE IF EXISTS usage_minute_stats_backfilled;
DROP TABLE IF EXISTS usage_minute_stats;
DROP TABLE IF EXISTS usage_minute_scope_stats;
DROP TABLE IF EXISTS usage_minute_model_stats;
DROP TABLE IF EXISTS usage_hourly_stats_backfilled;
DROP TABLE IF EXISTS usage_hourly_stats;
DROP TABLE IF EXISTS usage_hourly_scope_stats;
DROP TABLE IF EXISTS usage_hourly_model_stats;
DROP TABLE IF EXISTS usage_daily_stats_backfilled;
DROP TABLE IF EXISTS usage_daily_stats;
DROP TABLE IF EXISTS usage_daily_scope_stats;
DROP TABLE IF EXISTS usage_daily_model_stats;
DROP TABLE IF EXISTS usage_commit_jobs;

-- Upgrade path: rename usage_events -> requests when still on old schema.
SET @revlm_usage_events_exists := (
  SELECT COUNT(*) FROM information_schema.tables
  WHERE table_schema = DATABASE() AND table_name = 'usage_events'
);
SET @revlm_requests_exists := (
  SELECT COUNT(*) FROM information_schema.tables
  WHERE table_schema = DATABASE() AND table_name = 'requests'
);
SET @revlm_rename_sql := IF(
  @revlm_usage_events_exists > 0 AND @revlm_requests_exists = 0,
  'RENAME TABLE usage_events TO requests',
  'SELECT 1'
);
PREPARE revlm_rename_stmt FROM @revlm_rename_sql;
EXECUTE revlm_rename_stmt;
DEALLOCATE PREPARE revlm_rename_stmt;

CREATE TABLE IF NOT EXISTS request_totals (
  user_id BIGINT NOT NULL,
  token_id BIGINT NOT NULL,
  date DATE NOT NULL,
  requests BIGINT NOT NULL DEFAULT 0,
  input_tokens BIGINT NOT NULL DEFAULT 0,
  output_tokens BIGINT NOT NULL DEFAULT 0,
  cache_read_tokens BIGINT NOT NULL DEFAULT 0,
  cache_creation_tokens BIGINT NOT NULL DEFAULT 0,
  tokens BIGINT NOT NULL DEFAULT 0,
  usd DOUBLE NOT NULL DEFAULT 0,
  first_token_latency_sum BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY (user_id, token_id, date),
  KEY idx_request_totals_token_date (token_id, date),
  KEY idx_request_totals_user_date (user_id, date)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
