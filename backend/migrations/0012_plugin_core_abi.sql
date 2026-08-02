-- V1 preview builds used the name sdk_abi. The actual contract is the full
-- core ABI, so retain existing installations while correcting the column.
SET @revlm_has_sdk_abi := (
  SELECT COUNT(*)
  FROM information_schema.columns
  WHERE table_schema = DATABASE()
    AND table_name = 'plugin_installations'
    AND column_name = 'sdk_abi'
);
SET @revlm_plugin_abi_sql := IF(
  @revlm_has_sdk_abi = 1,
  'ALTER TABLE plugin_installations CHANGE COLUMN sdk_abi core_abi VARCHAR(128) NOT NULL',
  'SELECT 1'
);
PREPARE revlm_plugin_abi_stmt FROM @revlm_plugin_abi_sql;
EXECUTE revlm_plugin_abi_stmt;
DEALLOCATE PREPARE revlm_plugin_abi_stmt;
