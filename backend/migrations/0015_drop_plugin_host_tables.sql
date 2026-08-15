-- The plugin host no longer keeps installation state in the database. What is
-- installed is read from the packages directory; what is disabled or pending
-- uninstall lives in REVLM_PLUGIN_DIR/state.json. Migrations are now owned by
-- each plugin (revlm_plugin_migrate), so the core no longer tracks them either.
DROP TABLE IF EXISTS plugin_migrations;
DROP TABLE IF EXISTS plugin_installations;
