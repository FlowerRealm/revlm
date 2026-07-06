#include "store/migrations.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{

int expect(bool ok, const char *message)
{
    if (ok) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main()
{
    const std::vector<std::string> stmts =
        revlm::split_sql_statements("-- comment\nCREATE TABLE t(id INT);\nINSERT INTO t VALUES(1); ;\n");
    if (expect(stmts.size() == 2, "split should ignore empty statements") != 0 ||
        expect(stmts[0].find("CREATE TABLE") != std::string::npos, "split should keep first statement") != 0 ||
        expect(stmts[1] == "INSERT INTO t VALUES(1)", "split should trim statements") != 0) {
        return 1;
    }

    if (expect(revlm::is_mysql_implicit_commit_statement("-- x\nALTER TABLE t ADD c INT"),
               "ALTER should be implicit commit") != 0 ||
        expect(revlm::is_mysql_implicit_commit_statement("CREATE UNIQUE INDEX idx ON t(c)"),
               "CREATE UNIQUE INDEX should be implicit commit") != 0 ||
        expect(!revlm::is_mysql_implicit_commit_statement("INSERT INTO t VALUES(1)"),
               "INSERT should not be implicit commit") != 0 ||
        expect(revlm::is_mysql_dynamic_statement("PREPARE stmt FROM @ddl"), "PREPARE should be dynamic") != 0 ||
        expect(revlm::is_ephemeral_migration_statement("SELECT 1"), "SELECT should be ephemeral") != 0) {
        return 1;
    }

    if (expect(revlm::migration_statement_hash(" SELECT 1 ") == "42364a017b73ef516a0eca9827e6fa00623257ee",
               "statement hash should match SHA1 of trimmed SQL") != 0 ||
        expect(revlm::scoped_migration_lock_name("revlm.schema_migrations", "revlm_test") ==
                   "revlm.schema_migrations.533e01b371c8",
               "scoped migration lock should use first six SHA1 bytes") != 0) {
        return 1;
    }

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tmp-revlm-migrations-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream(dir / "0002_second.sql") << "SELECT 2;";
        std::ofstream(dir / "0001_first.sql") << "SELECT 1;";
        std::ofstream(dir / "README.txt") << "ignore";
    }
    const std::vector<revlm::MigrationEntry> migrations = revlm::load_migrations_from_dir(dir.string());
    std::filesystem::remove_all(dir);
    if (expect(migrations.size() == 2, "loader should include only sql files") != 0 ||
        expect(migrations[0].version == "0001_first.sql", "loader should sort by filename") != 0 ||
        expect(revlm::current_migration_signature(migrations) == "ee72b1e7d26e",
               "migration signature should be stable") != 0) {
        return 1;
    }

    return 0;
}
