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

CREATE TABLE `requests` (
  `id` bigint NOT NULL,
  `time` datetime NOT NULL,
  `endpoint` varchar(128) DEFAULT NULL,
  `method` varchar(16) DEFAULT NULL,
  `status_code` int NOT NULL DEFAULT '0',
  `latency_ms` int NOT NULL DEFAULT '0',
  `first_token_latency_ms` int NOT NULL DEFAULT '0',
  `error_class` varchar(64) DEFAULT NULL,
  `error_message` varchar(255) DEFAULT NULL,
  `user_id` bigint NOT NULL,
  `token_id` bigint NOT NULL,
  `channel_id` bigint NOT NULL DEFAULT '0',
  `status` varchar(16) NOT NULL DEFAULT 'committed',
  `model` varchar(128) DEFAULT NULL,
  `service_tier` varchar(32) DEFAULT NULL,
  `input_tokens` bigint DEFAULT NULL,
  `cache_read_tokens` bigint DEFAULT NULL,
  `cache_creation_5m_tokens` bigint DEFAULT NULL,
  `cache_creation_1h_tokens` bigint DEFAULT NULL,
  `output_tokens` bigint DEFAULT NULL,
  `tier_multiplier` double NOT NULL DEFAULT '1',
  `channel_multiplier` double NOT NULL DEFAULT '1',
  `is_stream` tinyint NOT NULL DEFAULT '0',
  PRIMARY KEY (`id`),
  KEY `idx_requests_time` (`time`),
  KEY `idx_requests_user_id` (`user_id`),
  KEY `idx_requests_token_id` (`token_id`),
  KEY `idx_requests_user_status_time` (`user_id`,`status`,`time`),
  KEY `idx_requests_token_time_id` (`token_id`,`time`,`id`),
  KEY `idx_requests_channel_time_id` (`channel_id`,`time`,`id`),
  KEY `idx_requests_model_time_id` (`model`,`time`,`id`),
  KEY `idx_requests_user_time_id` (`user_id`,`time`,`id`),
  KEY `time` (`time`,`channel_id`,`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `request_totals` (
  `user_id` bigint NOT NULL,
  `token_id` bigint NOT NULL,
  `date` date NOT NULL,
  `requests` bigint NOT NULL DEFAULT '0',
  `input_tokens` bigint NOT NULL DEFAULT '0',
  `output_tokens` bigint NOT NULL DEFAULT '0',
  `cache_read_tokens` bigint NOT NULL DEFAULT '0',
  `cache_creation_tokens` bigint NOT NULL DEFAULT '0',
  `tokens` bigint NOT NULL DEFAULT '0',
  `usd` double NOT NULL DEFAULT '0',
  `first_token_latency_sum` bigint NOT NULL DEFAULT '0',
  PRIMARY KEY (`user_id`,`token_id`,`date`),
  KEY `idx_request_totals_token_date` (`token_id`,`date`),
  KEY `idx_request_totals_user_date` (`user_id`,`date`)
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
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_users_email` (`email`),
  UNIQUE KEY `uk_users_username` (`username`),
  CONSTRAINT `chk_users_username_alnum` CHECK (regexp_like(`username`,_utf8mb4'^[A-Za-z0-9]+$'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
