#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"

namespace revlm
{

struct MigrationEntry {
    std::string version;
    std::string sql;
};

struct MigrationResult {
    int applied = 0;
    int total = 0;
};

std::vector<MigrationEntry> load_migrations_from_dir(const std::string &dir);
std::vector<std::string> split_sql_statements(std::string_view sql);
bool is_mysql_implicit_commit_statement(std::string_view statement);
bool is_mysql_dynamic_statement(std::string_view statement);
bool is_ephemeral_migration_statement(std::string_view statement);
std::string migration_statement_hash(std::string_view statement);
std::string scoped_migration_lock_name(std::string_view base, std::string_view scope);
std::string current_migration_signature(const std::vector<MigrationEntry> &migrations);

MigrationResult apply_migrations(const Config &config);
MigrationResult apply_migrations(const std::string &dsn, const std::string &migrations_dir,
                                 const std::string &lock_name, int lock_timeout_seconds);

} // namespace revlm
