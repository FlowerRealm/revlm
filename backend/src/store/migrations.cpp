#include "store/migrations.hpp"

#include "store/mysql.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

std::string uppercase(std::string value)
{
    for (char &ch : value) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string hex_bytes(const unsigned char *bytes, size_t size)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) {
        out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return out.str();
}

std::string sha1_hex(std::string_view value)
{
    std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
    SHA1(reinterpret_cast<const unsigned char *>(value.data()), value.size(), digest.data());
    return hex_bytes(digest.data(), digest.size());
}

std::string strip_leading_sql_comments(std::string_view input)
{
    std::string stmt = trim_ascii(input);
    while (!stmt.empty()) {
        if (stmt.rfind("--", 0) == 0 || stmt.rfind("#", 0) == 0) {
            const size_t newline = stmt.find('\n');
            if (newline == std::string::npos) {
                return {};
            }
            stmt = trim_ascii(std::string_view{ stmt }.substr(newline + 1));
            continue;
        }
        if (stmt.rfind("/*", 0) == 0) {
            const size_t end = stmt.find("*/");
            if (end == std::string::npos) {
                return {};
            }
            stmt = trim_ascii(std::string_view{ stmt }.substr(end + 2));
            continue;
        }
        break;
    }
    return stmt;
}

std::vector<std::string> statement_words(std::string_view statement)
{
    const std::string stripped = strip_leading_sql_comments(statement);
    std::vector<std::string> words;
    std::string current;
    for (char ch : stripped) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isspace(c) != 0 || ch == '(' || ch == '`') {
            if (!current.empty()) {
                words.push_back(uppercase(std::move(current)));
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        words.push_back(uppercase(std::move(current)));
    }
    return words;
}

bool has_suffix(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

std::filesystem::path resolve_migrations_dir(std::string_view dir)
{
    namespace fs = std::filesystem;
    const fs::path requested = fs::path(trim_ascii(dir));
    std::error_code ec;
    if (requested.empty()) {
        return requested;
    }
    if (fs::is_directory(requested, ec) && !ec) {
        return requested;
    }
    if (requested.is_absolute()) {
        return requested;
    }
#ifdef REVLM_SOURCE_DIR
    ec.clear();
    const fs::path source_relative = fs::path(REVLM_SOURCE_DIR) / requested;
    if (fs::is_directory(source_relative, ec) && !ec) {
        return source_relative;
    }
#endif
    return requested;
}

std::string current_database_name(MysqlConnection &conn)
{
    return trim_ascii(conn.query_one("SELECT DATABASE()").value_or(""));
}

std::string resolve_lock_name(MysqlConnection &conn, std::string_view requested)
{
    const std::string lock = trim_ascii(requested);
    if (!lock.empty()) {
        return lock;
    }
    const std::string db = current_database_name(conn);
    if (db.empty()) {
        return "revlm.schema_migrations";
    }
    return scoped_migration_lock_name("revlm.schema_migrations", db);
}

class MigrationLock {
public:
    MigrationLock(MysqlConnection &conn, std::string lock_name, int timeout_seconds)
        : conn_(conn)
        , lock_name_(std::move(lock_name))
    {
        if (lock_name_.empty()) {
            throw std::invalid_argument("migration lock name must not be empty");
        }
        if (timeout_seconds < 0) {
            throw std::invalid_argument("migration lock timeout must not be negative");
        }
        const std::string sql =
            "SELECT GET_LOCK(" + sql_quote(lock_name_) + ", " + std::to_string(timeout_seconds) + ")";
        const std::string got = trim_ascii(conn_.query_one(sql).value_or(""));
        if (got == "1") {
            locked_ = true;
            return;
        }
        if (got == "0") {
            throw std::runtime_error("waiting for migration lock timed out: " + lock_name_);
        }
        throw std::runtime_error("GET_LOCK failed for migration lock: " + lock_name_);
    }

    MigrationLock(const MigrationLock &) = delete;
    MigrationLock &operator=(const MigrationLock &) = delete;

    ~MigrationLock()
    {
        if (!locked_) {
            return;
        }
        try {
            (void)conn_.query_one("SELECT RELEASE_LOCK(" + sql_quote(lock_name_) + ")");
        } catch (const std::exception &) {
        }
    }

private:
    MysqlConnection &conn_;
    std::string lock_name_;
    bool locked_ = false;
};

void ensure_migration_tables(MysqlConnection &conn)
{
    conn.exec(R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations (
  version VARCHAR(255) PRIMARY KEY,
  applied_at DATETIME NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
)SQL");
    conn.exec(R"SQL(
CREATE TABLE IF NOT EXISTS schema_migration_steps (
  version VARCHAR(255) NOT NULL,
  step_index INT NOT NULL,
  statement_hash CHAR(40) NOT NULL,
  applied_at DATETIME NOT NULL,
  PRIMARY KEY (version, step_index)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
)SQL");
}

bool migration_applied(MysqlConnection &conn, std::string_view version)
{
    const std::string sql = "SELECT version FROM schema_migrations WHERE version=" + sql_quote(version);
    return conn.query_one(sql).has_value();
}

bool is_reentry_error_code(unsigned int code)
{
    switch (code) {
    case 1050:
    case 1051:
    case 1060:
    case 1061:
    case 1068:
    case 1091:
    case 1359:
    case 1360:
    case 1826:
    case 3822:
        return true;
    default:
        return false;
    }
}

void exec_statement(MysqlConnection &conn, std::string_view statement)
{
    try {
        conn.exec(statement);
    } catch (const std::exception &err) {
        throw std::runtime_error(std::string{ err.what() });
    }
}

bool exec_ddl_allow_reentry(MysqlConnection &conn, std::string_view statement, bool dynamic)
{
    try {
        conn.exec(statement);
        return false;
    } catch (const std::exception &) {
        const unsigned int code = conn.last_errno();
        if ((dynamic || is_mysql_implicit_commit_statement(statement)) && is_reentry_error_code(code)) {
            return true;
        }
        throw;
    }
}

bool migration_step_applied(MysqlConnection &conn, std::string_view version, int step_index, std::string_view statement)
{
    const std::string sql = "SELECT statement_hash FROM schema_migration_steps WHERE version=" + sql_quote(version) +
                            " AND step_index=" + std::to_string(step_index);
    const auto got = conn.query_one(sql);
    if (!got.has_value()) {
        return false;
    }
    const std::string want = migration_statement_hash(statement);
    if (*got != want) {
        throw std::runtime_error("migration step hash mismatch for " + std::string{ version } + "[" +
                                 std::to_string(step_index) + "]");
    }
    return true;
}

void record_migration_step(MysqlConnection &conn, std::string_view version, int step_index, std::string_view statement)
{
    const std::string sql =
        "INSERT INTO schema_migration_steps(version, step_index, statement_hash, applied_at) VALUES(" +
        sql_quote(version) + ", " + std::to_string(step_index) + ", " + sql_quote(migration_statement_hash(statement)) +
        ", CURRENT_TIMESTAMP)";
    conn.exec(sql);
}

void apply_transactional_step(MysqlConnection &conn, std::string_view version, int step_index,
                              std::string_view statement)
{
    DbTransaction tr(conn);
    exec_statement(conn, statement);
    record_migration_step(conn, version, step_index, statement);
    tr.commit();
}

bool requires_non_transactional_migration(const std::vector<std::string> &statements)
{
    for (const std::string &statement : statements) {
        if (is_mysql_implicit_commit_statement(statement) || is_mysql_dynamic_statement(statement)) {
            return true;
        }
    }
    return false;
}

void finish_migration(MysqlConnection &conn, std::string_view version)
{
    conn.exec("INSERT INTO schema_migrations(version, applied_at) VALUES(" + sql_quote(version) +
              ", CURRENT_TIMESTAMP)");
    conn.exec("DELETE FROM schema_migration_steps WHERE version=" + sql_quote(version));
}

void apply_non_transactional_migration(MysqlConnection &conn, std::string_view version,
                                       const std::vector<std::string> &statements)
{
    for (size_t i = 0; i < statements.size(); ++i) {
        const int step_index = static_cast<int>(i + 1);
        const std::string &statement = statements[i];
        if (is_ephemeral_migration_statement(statement)) {
            (void)exec_ddl_allow_reentry(conn, statement, is_mysql_dynamic_statement(statement));
            continue;
        }
        if (migration_step_applied(conn, version, step_index, statement)) {
            continue;
        }
        if (is_mysql_implicit_commit_statement(statement)) {
            (void)exec_ddl_allow_reentry(conn, statement, false);
            record_migration_step(conn, version, step_index, statement);
            continue;
        }
        apply_transactional_step(conn, version, step_index, statement);
    }
    finish_migration(conn, version);
}

void apply_transactional_migration(MysqlConnection &conn, std::string_view version,
                                   const std::vector<std::string> &statements)
{
    DbTransaction tr(conn);
    for (const std::string &statement : statements) {
        exec_statement(conn, statement);
    }
    conn.exec("INSERT INTO schema_migrations(version, applied_at) VALUES(" + sql_quote(version) +
              ", CURRENT_TIMESTAMP)");
    tr.commit();
}

void apply_migration(MysqlConnection &conn, const MigrationEntry &migration)
{
    const std::vector<std::string> statements = split_sql_statements(migration.sql);
    if (requires_non_transactional_migration(statements)) {
        apply_non_transactional_migration(conn, migration.version, statements);
        return;
    }
    apply_transactional_migration(conn, migration.version, statements);
}

} // namespace

std::vector<MigrationEntry> load_migrations_from_dir(const std::string &dir)
{
    namespace fs = std::filesystem;
    std::vector<MigrationEntry> migrations;
    const fs::path resolved = resolve_migrations_dir(dir);
    std::error_code ec;
    if (!fs::is_directory(resolved, ec) || ec) {
        throw std::runtime_error("migrations directory not found: " + dir);
    }
    for (const fs::directory_entry &entry : fs::directory_iterator(resolved)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path path = entry.path();
        const std::string name = path.filename().string();
        if (!has_suffix(name, ".sql")) {
            continue;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("read migration failed: " + path.string());
        }
        std::string body{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        migrations.push_back(MigrationEntry{ name, std::move(body) });
    }
    std::sort(migrations.begin(), migrations.end(),
              [](const MigrationEntry &a, const MigrationEntry &b) { return a.version < b.version; });
    return migrations;
}

std::vector<std::string> split_sql_statements(std::string_view sql)
{
    std::vector<std::string> statements;
    size_t begin = 0;
    for (size_t i = 0; i <= sql.size(); ++i) {
        if (i != sql.size() && sql[i] != ';') {
            continue;
        }
        const std::string stmt = trim_ascii(sql.substr(begin, i - begin));
        if (!stmt.empty()) {
            statements.push_back(stmt);
        }
        begin = i + 1;
    }
    return statements;
}

bool is_mysql_implicit_commit_statement(std::string_view statement)
{
    const std::vector<std::string> words = statement_words(statement);
    if (words.empty()) {
        return false;
    }
    const std::string &first = words[0];
    if (first == "ALTER" || first == "TRUNCATE" || first == "RENAME") {
        return true;
    }
    if (words.size() < 2) {
        return false;
    }
    const std::string &second = words[1];
    if (first == "CREATE") {
        if ((second == "UNIQUE" || second == "FULLTEXT" || second == "SPATIAL") && words.size() >= 3) {
            return words[2] == "INDEX";
        }
        return second == "DATABASE" || second == "EVENT" || second == "FUNCTION" || second == "INDEX" ||
               second == "PROCEDURE" || second == "TABLE" || second == "TRIGGER" || second == "VIEW";
    }
    if (first == "DROP") {
        return second == "DATABASE" || second == "EVENT" || second == "FUNCTION" || second == "INDEX" ||
               second == "PROCEDURE" || second == "TABLE" || second == "TRIGGER" || second == "VIEW";
    }
    return false;
}

bool is_mysql_dynamic_statement(std::string_view statement)
{
    const std::vector<std::string> words = statement_words(statement);
    return !words.empty() && (words[0] == "PREPARE" || words[0] == "EXECUTE" || words[0] == "DEALLOCATE");
}

bool is_ephemeral_migration_statement(std::string_view statement)
{
    const std::vector<std::string> words = statement_words(statement);
    if (words.empty()) {
        return true;
    }
    return words[0] == "SET" || words[0] == "SELECT" || is_mysql_dynamic_statement(statement);
}

std::string migration_statement_hash(std::string_view statement)
{
    return sha1_hex(trim_ascii(statement));
}

std::string scoped_migration_lock_name(std::string_view base, std::string_view scope)
{
    const std::string clean_scope = trim_ascii(scope);
    if (clean_scope.empty()) {
        return std::string{ base };
    }
    const std::string scope_hash = sha1_hex(clean_scope);
    return std::string{ base } + "." + scope_hash.substr(0, 12);
}

std::string current_migration_signature(const std::vector<MigrationEntry> &migrations)
{
    std::string payload;
    for (const MigrationEntry &migration : migrations) {
        payload.append(migration.version);
        payload.push_back('\0');
        payload.append(migration.sql);
        payload.push_back('\0');
    }
    return sha1_hex(payload).substr(0, 12);
}

MigrationResult apply_migrations(const Config &config)
{
    return apply_migrations(config.db_dsn, config.migrations_dir, config.db_migration_lock_name,
                            config.db_migration_lock_timeout_seconds);
}

MigrationResult apply_migrations(const std::string &dsn, const std::string &migrations_dir,
                                 const std::string &lock_name, int lock_timeout_seconds)
{
    const std::vector<MigrationEntry> migrations = load_migrations_from_dir(migrations_dir);
    MysqlConnection conn(parse_mysql_dsn(dsn));
    const std::string resolved_lock = resolve_lock_name(conn, lock_name);
    MigrationLock lock(conn, resolved_lock, lock_timeout_seconds);
    ensure_migration_tables(conn);

    MigrationResult result;
    result.total = static_cast<int>(migrations.size());
    for (const MigrationEntry &migration : migrations) {
        if (migration_applied(conn, migration.version)) {
            continue;
        }
        apply_migration(conn, migration);
        ++result.applied;
    }
    return result;
}

} // namespace revlm
