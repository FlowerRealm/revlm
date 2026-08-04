-- ADR-0004: 核心请求表收敛。协议 token 统计移入 token_details JSON；
-- 协议 tier 字段删除；channel_multiplier 改名 ChannelGroupMultiplier。
-- request_totals 只保留请求数/最终金额/首 token 延迟通用聚合。

ALTER TABLE requests
  DROP COLUMN input_tokens,
  DROP COLUMN output_tokens,
  DROP COLUMN cache_read_tokens,
  DROP COLUMN cache_creation_1h_tokens,
  DROP COLUMN cache_creation_5m_tokens,
  DROP COLUMN tier_multiplier,
  DROP COLUMN service_tier,
  CHANGE COLUMN channel_multiplier channel_group_multiplier DOUBLE NOT NULL DEFAULT 1.0,
  ADD COLUMN token_details TEXT NULL;

ALTER TABLE request_totals
  DROP COLUMN input_tokens,
  DROP COLUMN output_tokens,
  DROP COLUMN cache_read_tokens,
  DROP COLUMN cache_creation_tokens,
  DROP COLUMN tokens;
