-- ADR-0005: Channel 收敛为纯上游端点；type 与 price_multiplier 上移 ChannelGroup。
-- Channel 删除 type/price_multiplier/config_json；ChannelGroup 新增必填 type。
-- 旧 channels.type 不回填（无现网数据，见 ADR-0005 迁移节）。

ALTER TABLE channel_groups ADD COLUMN type VARCHAR(64) NOT NULL DEFAULT '';

ALTER TABLE channels DROP COLUMN type;
ALTER TABLE channels DROP COLUMN price_multiplier;
ALTER TABLE channels DROP COLUMN config_json;
