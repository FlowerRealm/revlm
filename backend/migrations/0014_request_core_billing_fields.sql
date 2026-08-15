ALTER TABLE requests DROP COLUMN input_tokens;
ALTER TABLE requests DROP COLUMN output_tokens;
ALTER TABLE requests DROP COLUMN cache_read_tokens;
ALTER TABLE requests DROP COLUMN cache_creation_1h_tokens;
ALTER TABLE requests DROP COLUMN cache_creation_5m_tokens;
ALTER TABLE requests DROP COLUMN tier_multiplier;
ALTER TABLE requests DROP COLUMN service_tier;
ALTER TABLE requests DROP COLUMN is_stream;
ALTER TABLE requests DROP COLUMN error_class;
ALTER TABLE requests CHANGE COLUMN channel_multiplier channel_group_multiplier DOUBLE NOT NULL DEFAULT 1.0;
ALTER TABLE requests ADD COLUMN usage_details TEXT NOT NULL DEFAULT '{}';

ALTER TABLE request_totals DROP COLUMN input_tokens;
ALTER TABLE request_totals DROP COLUMN output_tokens;
ALTER TABLE request_totals DROP COLUMN cache_read_tokens;
ALTER TABLE request_totals DROP COLUMN cache_creation_tokens;
ALTER TABLE request_totals DROP COLUMN tokens;
