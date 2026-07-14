#include "store/schema.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

#include <odb/schema-catalog.hxx>
#include <odb/transaction.hxx>

#include "embedded_migrations.hxx"
#include "store/database.hpp"

namespace revlm
{
namespace
{

bool table_exists(odb::database &db, std::string_view table)
{
    const std::string sql = "SELECT 1 FROM information_schema.tables "
                            "WHERE table_schema = DATABASE() AND table_name = " +
                            sql_quote(db, table) + " LIMIT 1";
    return sql_query_one(db, sql).value_or("") == "1";
}

void ensure_migrations_table(odb::database &db)
{
    sql_exec(db, "CREATE TABLE IF NOT EXISTS schema_migrations ("
                 "  version INT NOT NULL PRIMARY KEY,"
                 "  name VARCHAR(255) NOT NULL,"
                 "  applied_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP"
                 ")");
}

bool migration_applied(odb::database &db, int version)
{
    const std::string sql = "SELECT 1 FROM schema_migrations WHERE version = " + std::to_string(version) + " LIMIT 1";
    return sql_query_one(db, sql).value_or("") == "1";
}

void record_migration(odb::database &db, int version, std::string_view name)
{
    sql_exec(db, "INSERT INTO schema_migrations (version, name) VALUES (" + std::to_string(version) + ", " +
                     sql_quote(db, name) + ")");
}

bool migrations_table_empty(odb::database &db)
{
    return sql_query_one(db, "SELECT COUNT(*) FROM schema_migrations").value_or("0") == "0";
}

void apply_pending_migrations(odb::database &db)
{
    using revlm::embedded_migrations::kMigrationCount;
    using revlm::embedded_migrations::kMigrations;

    for (std::size_t i = 0; i < kMigrationCount; ++i) {
        const auto &m = kMigrations[i];
        if (migration_applied(db, m.version)) {
            continue;
        }
        // Skip empty / comment-only bodies safely: still record the version.
        std::string_view sql = m.sql;
        while (!sql.empty() &&
               (sql.front() == ' ' || sql.front() == '\t' || sql.front() == '\n' || sql.front() == '\r')) {
            sql.remove_prefix(1);
        }
        // Strip leading SQL line comments for the emptiness check.
        while (sql.starts_with("--")) {
            const auto nl = sql.find('\n');
            if (nl == std::string_view::npos) {
                sql = {};
                break;
            }
            sql.remove_prefix(nl + 1);
            while (!sql.empty() &&
                   (sql.front() == ' ' || sql.front() == '\t' || sql.front() == '\n' || sql.front() == '\r')) {
                sql.remove_prefix(1);
            }
        }
        if (!sql.empty()) {
            sql_exec(db, m.sql);
        }
        record_migration(db, m.version, m.name);
    }
}

} // namespace

void ensure_schema(odb::database &db)
{
    if (embedded_migrations::kMigrationCount == 0) {
        throw std::runtime_error("ensure_schema: no embedded SQL migrations");
    }
    if (embedded_migrations::kMigrations[0].version != 1) {
        throw std::runtime_error("ensure_schema: first migration must be version 1 (baseline)");
    }

    {
        odb::transaction t(db.begin());
        ensure_migrations_table(db);

        const bool has_product = table_exists(db, "users");
        if (!has_product) {
            odb::schema_catalog::create_schema(db, "", false);
            if (!migration_applied(db, 1)) {
                record_migration(db, 1, embedded_migrations::kMigrations[0].name);
            }
        } else if (migrations_table_empty(db)) {
            // Existing DB from before versioned migrations: baseline already present.
            record_migration(db, 1, embedded_migrations::kMigrations[0].name);
        }

        apply_pending_migrations(db);
        t.commit();
    }
}

} // namespace revlm
