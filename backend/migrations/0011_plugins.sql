ALTER TABLE channels ADD COLUMN config_json TEXT NOT NULL DEFAULT '{}';

CREATE TABLE IF NOT EXISTS plugin_installations (
  plugin_id VARCHAR(128) NOT NULL,
  version VARCHAR(128) NOT NULL,
  display_name VARCHAR(255) NOT NULL,
  core_abi VARCHAR(128) NOT NULL,
  status VARCHAR(32) NOT NULL,
  package_path TEXT NOT NULL,
  target_os VARCHAR(32) NOT NULL,
  target_arch VARCHAR(32) NOT NULL,
  enabled TINYINT(1) NOT NULL DEFAULT 1,
  system_plugin TINYINT(1) NOT NULL DEFAULT 0,
  error_message TEXT NOT NULL,
  installed_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (plugin_id),
  KEY plugin_installations_status (status)
);

CREATE TABLE IF NOT EXISTS plugin_migrations (
  plugin_id VARCHAR(128) NOT NULL,
  migration_id VARCHAR(255) NOT NULL,
  applied_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (plugin_id, migration_id)
);
