ALTER TABLE user_tokens CHANGE COLUMN channel_id channel_group_id BIGINT NOT NULL DEFAULT 0;
