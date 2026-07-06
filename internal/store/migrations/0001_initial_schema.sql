-- 0001_initial_schema.sql: consolidated Revlm schema.

CREATE TABLE `app_settings` (
  `key` varchar(64) NOT NULL,
  `value` varchar(255) NOT NULL,
  `created_at` datetime NOT NULL,
  `updated_at` datetime NOT NULL,
  PRIMARY KEY (`key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `channel_group_members` (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `channel_group_id` bigint NOT NULL,
  `channel_id` bigint NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `channel_group_id` (`channel_group_id`,`channel_id`),
  KEY `channel_group_id_2` (`channel_group_id`,`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `channel_groups` (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `name` varchar(64) NOT NULL,
  `description` varchar(255) DEFAULT NULL,
  `price_multiplier` decimal(25,6) NOT NULL DEFAULT '1.000000',
  `status` tinyint NOT NULL DEFAULT '1',
  PRIMARY KEY (`id`),
  UNIQUE KEY `name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `channels` (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `type` tinyint NOT NULL,
  `name` varchar(64) NOT NULL,
  `status` tinyint NOT NULL DEFAULT '1',
  `priority` int NOT NULL DEFAULT '0',
  `base_url` varchar(255) NOT NULL DEFAULT '',
  `api_key` varchar(8192) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `session_bindings` (
  `user_id` bigint NOT NULL,
  `route_key_hash` varchar(128) NOT NULL,
  `payload_json` mediumtext NOT NULL,
  `expires_at` datetime NOT NULL,
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`user_id`,`route_key_hash`),
  KEY `idx_session_bindings_expires_at` (`expires_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `token_channel_groups` (
  `token_id` bigint NOT NULL,
  `channel_group_id` bigint NOT NULL,
  `priority` int NOT NULL DEFAULT '0',
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`token_id`,`channel_group_id`),
  KEY `idx_token_channel_groups_token_id` (`token_id`),
  KEY `idx_token_channel_groups_group_priority` (`channel_group_id`,`priority`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `token_model_mappings` (
  `token_id` bigint NOT NULL,
  `input_model` varchar(128) NOT NULL,
  `target_model` varchar(128) NOT NULL,
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`token_id`,`input_model`),
  KEY `idx_token_model_mappings_target_model` (`target_model`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_commit_jobs` (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `request_id` varchar(64) NOT NULL,
  `user_id` bigint NOT NULL,
  `token_id` bigint NOT NULL,
  `state` varchar(32) NOT NULL,
  `lease_token` varchar(64) DEFAULT NULL,
  `lease_until` datetime DEFAULT NULL,
  `attempts` int NOT NULL DEFAULT '0',
  `payload_json` json NOT NULL,
  `created_at` datetime NOT NULL,
  `updated_at` datetime NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_usage_commit_jobs_request_id` (`request_id`),
  KEY `idx_usage_commit_jobs_state_updated` (`state`,`updated_at`,`id`),
  KEY `idx_usage_commit_jobs_lease_until` (`lease_until`),
  KEY `idx_usage_commit_jobs_state_lease_token` (`state`,`lease_token`),
  KEY `idx_usage_commit_jobs_state_lease_until` (`state`,`lease_until`,`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_daily_model_stats` (
  `stat_date` date NOT NULL,
  `user_id` bigint NOT NULL,
  `model` varchar(128) NOT NULL,
  `requests` bigint NOT NULL DEFAULT '0',
  `input_tokens` bigint NOT NULL DEFAULT '0',
  `output_tokens` bigint NOT NULL DEFAULT '0',
  `committed_usd` decimal(20,6) NOT NULL DEFAULT '0.000000',
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`stat_date`,`user_id`,`model`),
  KEY `idx_daily_model_user_date` (`user_id`,`stat_date`,`model`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_daily_scope_stats` (
  `stat_date` date NOT NULL,
  `scope_type` varchar(16) NOT NULL,
  `scope_id` bigint NOT NULL,
  `requests` bigint NOT NULL DEFAULT '0',
  `input_tokens` bigint NOT NULL DEFAULT '0',
  `output_tokens` bigint NOT NULL DEFAULT '0',
  `cache_read_input_tokens` bigint NOT NULL DEFAULT '0',
  `cache_creation_input_tokens` bigint NOT NULL DEFAULT '0',
  `committed_usd` decimal(20,6) NOT NULL DEFAULT '0.000000',
  `first_token_samples` bigint NOT NULL DEFAULT '0',
  `first_token_latency_sum` bigint NOT NULL DEFAULT '0',
  `output_tokens_for_tps` bigint NOT NULL DEFAULT '0',
  `decode_latency_sum` bigint NOT NULL DEFAULT '0',
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`stat_date`,`scope_type`,`scope_id`),
  KEY `idx_daily_scope_lookup` (`scope_type`,`scope_id`,`stat_date`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_daily_stats` (
  `stat_date` date NOT NULL,
  `user_id` bigint NOT NULL,
  `token_id` bigint NOT NULL,
  `upstream_channel_id` bigint NOT NULL DEFAULT '0',
  `requests` bigint NOT NULL DEFAULT '0',
  `input_tokens` bigint NOT NULL DEFAULT '0',
  `output_tokens` bigint NOT NULL DEFAULT '0',
  `cache_read_input_tokens` bigint NOT NULL DEFAULT '0',
  `cache_creation_input_tokens` bigint NOT NULL DEFAULT '0',
  `committed_usd` decimal(20,6) NOT NULL DEFAULT '0.000000',
  `first_token_samples` bigint NOT NULL DEFAULT '0',
  `first_token_latency_sum` bigint NOT NULL DEFAULT '0',
  `output_tokens_for_tps` bigint NOT NULL DEFAULT '0',
  `decode_latency_sum` bigint NOT NULL DEFAULT '0',
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`stat_date`,`user_id`,`token_id`,`upstream_channel_id`),
  KEY `idx_daily_user_date` (`user_id`,`stat_date`),
  KEY `idx_daily_token_date` (`token_id`,`stat_date`),
  KEY `idx_daily_channel_date` (`upstream_channel_id`,`stat_date`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_daily_stats_backfilled` (
  `stat_date` date NOT NULL,
  `built_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`stat_date`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_events` (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `time` datetime NOT NULL,
  `request_id` varchar(64) NOT NULL,
  `endpoint` varchar(128) DEFAULT NULL,
  `method` varchar(16) DEFAULT NULL,
  `status_code` int NOT NULL DEFAULT '0',
  `latency_ms` int NOT NULL DEFAULT '0',
  `first_token_latency_ms` int NOT NULL DEFAULT '0',
  `error_class` varchar(64) DEFAULT NULL,
  `error_message` varchar(255) DEFAULT NULL,
  `user_id` bigint NOT NULL,
  `token_id` bigint NOT NULL,
  `channel_id` bigint DEFAULT NULL,
  `state` varchar(16) NOT NULL,
  `model` varchar(128) DEFAULT NULL,
  `forwarded_model` varchar(128) DEFAULT NULL,
  `upstream_response_model` varchar(128) DEFAULT NULL,
  `requested_service_tier` varchar(32) DEFAULT NULL,
  `service_tier` varchar(32) DEFAULT NULL,
  `service_tier_downgrade_reason` varchar(64) DEFAULT NULL,
  `input_tokens` bigint DEFAULT NULL,
  `cache_read_input_tokens` bigint DEFAULT NULL,
  `cache_creation_input_tokens` bigint DEFAULT NULL,
  `cache_creation_1h_input_tokens` bigint DEFAULT NULL,
  `output_tokens` bigint DEFAULT NULL,
  `committed_usd` decimal(20,6) NOT NULL DEFAULT '0.000000',
  `price_multiplier` decimal(20,6) NOT NULL DEFAULT '1.000000',
  `price_multiplier_group` decimal(20,6) NOT NULL DEFAULT '1.000000',
  `price_multiplier_payment` decimal(20,6) NOT NULL DEFAULT '1.000000',
  `price_multiplier_group_name` text,
  `is_stream` tinyint NOT NULL DEFAULT '0',
  `request_bytes` bigint NOT NULL DEFAULT '0',
  `response_bytes` bigint NOT NULL DEFAULT '0',
  `created_at` datetime NOT NULL,
  `updated_at` datetime NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_usage_events_request_id` (`request_id`),
  KEY `idx_usage_events_time` (`time`),
  KEY `idx_usage_events_user_id` (`user_id`),
  KEY `idx_usage_events_token_id` (`token_id`),
  KEY `idx_usage_events_user_state_time` (`user_id`,`state`,`time`),
  KEY `idx_usage_events_token_time_id` (`token_id`,`time`,`id`),
  KEY `idx_usage_events_upstream_channel_time_id` (`channel_id`,`time`,`id`),
  KEY `idx_usage_events_model_time_id` (`model`,`time`,`id`),
  KEY `idx_usage_events_credential_time` (`time`),
  KEY `idx_usage_events_user_time_id` (`user_id`,`time`,`id`),
  KEY `time` (`time`,`channel_id`,`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_hourly_model_stats` (
  `stat_hour` datetime NOT NULL,
  `user_id` bigint NOT NULL,
  `model` varchar(128) NOT NULL,
  `requests` bigint NOT NULL DEFAULT '0',
  `input_tokens` bigint NOT NULL DEFAULT '0',
  `output_tokens` bigint NOT NULL DEFAULT '0',
  `committed_usd` decimal(20,6) NOT NULL DEFAULT '0.000000',
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`stat_hour`,`user_id`,`model`),
  KEY `idx_hourly_model_user_hour` (`user_id`,`stat_hour`,`model`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_hourly_scope_stats` (
  `stat_hour` datetime NOT NULL,
  `scope_type` varchar(16) NOT NULL,
  `scope_id` bigint NOT NULL,
  `requests` bigint NOT NULL DEFAULT '0',
  `input_tokens` bigint NOT NULL DEFAULT '0',
  `output_tokens` bigint NOT NULL DEFAULT '0',
  `cache_read_input_tokens` bigint NOT NULL DEFAULT '0',
  `cache_creation_input_tokens` bigint NOT NULL DEFAULT '0',
  `committed_usd` decimal(20,6) NOT NULL DEFAULT '0.000000',
  `first_token_samples` bigint NOT NULL DEFAULT '0',
  `first_token_latency_sum` bigint NOT NULL DEFAULT '0',
  `output_tokens_for_tps` bigint NOT NULL DEFAULT '0',
  `decode_latency_sum` bigint NOT NULL DEFAULT '0',
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`stat_hour`,`scope_type`,`scope_id`),
  KEY `idx_hourly_scope_lookup` (`scope_type`,`scope_id`,`stat_hour`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_hourly_stats` (
  `stat_hour` datetime NOT NULL,
  `user_id` bigint NOT NULL,
  `token_id` bigint NOT NULL,
  `upstream_channel_id` bigint NOT NULL DEFAULT '0',
  `requests` bigint NOT NULL DEFAULT '0',
  `input_tokens` bigint NOT NULL DEFAULT '0',
  `output_tokens` bigint NOT NULL DEFAULT '0',
  `cache_read_input_tokens` bigint NOT NULL DEFAULT '0',
  `cache_creation_input_tokens` bigint NOT NULL DEFAULT '0',
  `committed_usd` decimal(20,6) NOT NULL DEFAULT '0.000000',
  `first_token_samples` bigint NOT NULL DEFAULT '0',
  `first_token_latency_sum` bigint NOT NULL DEFAULT '0',
  `output_tokens_for_tps` bigint NOT NULL DEFAULT '0',
  `decode_latency_sum` bigint NOT NULL DEFAULT '0',
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`stat_hour`,`user_id`,`token_id`,`upstream_channel_id`),
  KEY `idx_hourly_user_hour` (`user_id`,`stat_hour`),
  KEY `idx_hourly_token_hour` (`token_id`,`stat_hour`),
  KEY `idx_hourly_channel_hour` (`upstream_channel_id`,`stat_hour`),
  KEY `idx_hourly_hour` (`stat_hour`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_hourly_stats_backfilled` (
  `stat_hour` datetime NOT NULL,
  `built_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`stat_hour`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_minute_model_stats` (
  `stat_minute` datetime NOT NULL,
  `user_id` bigint NOT NULL,
  `model` varchar(128) NOT NULL,
  `requests` bigint NOT NULL DEFAULT '0',
  `input_tokens` bigint NOT NULL DEFAULT '0',
  `output_tokens` bigint NOT NULL DEFAULT '0',
  `committed_usd` decimal(20,6) NOT NULL DEFAULT '0.000000',
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`stat_minute`,`user_id`,`model`),
  KEY `idx_minute_model_user_minute` (`user_id`,`stat_minute`,`model`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_minute_scope_stats` (
  `stat_minute` datetime NOT NULL,
  `scope_type` varchar(16) NOT NULL,
  `scope_id` bigint NOT NULL,
  `requests` bigint NOT NULL DEFAULT '0',
  `input_tokens` bigint NOT NULL DEFAULT '0',
  `output_tokens` bigint NOT NULL DEFAULT '0',
  `cache_read_input_tokens` bigint NOT NULL DEFAULT '0',
  `cache_creation_input_tokens` bigint NOT NULL DEFAULT '0',
  `committed_usd` decimal(20,6) NOT NULL DEFAULT '0.000000',
  `first_token_samples` bigint NOT NULL DEFAULT '0',
  `first_token_latency_sum` bigint NOT NULL DEFAULT '0',
  `output_tokens_for_tps` bigint NOT NULL DEFAULT '0',
  `decode_latency_sum` bigint NOT NULL DEFAULT '0',
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`stat_minute`,`scope_type`,`scope_id`),
  KEY `idx_minute_scope_lookup` (`scope_type`,`scope_id`,`stat_minute`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_minute_stats` (
  `stat_minute` datetime NOT NULL,
  `user_id` bigint NOT NULL,
  `token_id` bigint NOT NULL,
  `upstream_channel_id` bigint NOT NULL DEFAULT '0',
  `requests` bigint NOT NULL DEFAULT '0',
  `input_tokens` bigint NOT NULL DEFAULT '0',
  `output_tokens` bigint NOT NULL DEFAULT '0',
  `cache_read_input_tokens` bigint NOT NULL DEFAULT '0',
  `cache_creation_input_tokens` bigint NOT NULL DEFAULT '0',
  `committed_usd` decimal(20,6) NOT NULL DEFAULT '0.000000',
  `first_token_samples` bigint NOT NULL DEFAULT '0',
  `first_token_latency_sum` bigint NOT NULL DEFAULT '0',
  `output_tokens_for_tps` bigint NOT NULL DEFAULT '0',
  `decode_latency_sum` bigint NOT NULL DEFAULT '0',
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`stat_minute`,`user_id`,`token_id`,`upstream_channel_id`),
  KEY `idx_minute_user_minute` (`user_id`,`stat_minute`),
  KEY `idx_minute_token_minute` (`token_id`,`stat_minute`),
  KEY `idx_minute_channel_minute` (`upstream_channel_id`,`stat_minute`),
  KEY `idx_minute_minute` (`stat_minute`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `usage_minute_stats_backfilled` (
  `stat_minute` datetime NOT NULL,
  `built_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`stat_minute`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `user_balances` (
  `user_id` bigint NOT NULL,
  `usd` decimal(20,6) NOT NULL DEFAULT '0.000000',
  `created_at` datetime NOT NULL,
  `updated_at` datetime NOT NULL,
  PRIMARY KEY (`user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `user_tokens` (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `user_id` bigint NOT NULL,
  `name` varchar(128) DEFAULT NULL,
  `token_hash` varbinary(32) NOT NULL,
  `token_plain` varchar(255) DEFAULT NULL,
  `status` tinyint NOT NULL DEFAULT '1',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_user_tokens_hash` (`token_hash`),
  KEY `idx_user_tokens_user_id` (`user_id`),
  KEY `idx_user_tokens_user_id_name` (`user_id`,`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `users` (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `email` varchar(255) NOT NULL,
  `username` varchar(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
  `password_hash` varbinary(255) NOT NULL,
  `role` varchar(32) NOT NULL DEFAULT 'user',
  `status` tinyint NOT NULL DEFAULT '1',
  `created_at` datetime NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_users_email` (`email`),
  UNIQUE KEY `uk_users_username` (`username`),
  CONSTRAINT `chk_users_username_alnum` CHECK (regexp_like(`username`,_utf8mb4'^[A-Za-z0-9]+$'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
