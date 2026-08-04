-- Retained from the removed 0011_plugins.sql: channels.config_json is a
-- channel feature column (unbound plugin fields persist here), not plugin
-- host metadata. Kept as its own migration so dropping the plugin tables
-- does not lose the column on existing databases.
ALTER TABLE channels ADD COLUMN config_json TEXT NOT NULL DEFAULT '{}';
