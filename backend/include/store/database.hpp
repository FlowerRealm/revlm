#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <odb/database.hxx>
#include <odb/nullable.hxx>
#include <odb/transaction.hxx>

namespace revlm
{

struct ParsedMysqlDsn {
    std::string user;
    std::string password;
    std::string host = "127.0.0.1";
    unsigned int port = 3306;
    std::string database;
};

ParsedMysqlDsn parse_mysql_dsn(std::string_view raw);
std::unique_ptr<odb::database> make_database(const ParsedMysqlDsn &dsn);
std::unique_ptr<odb::database> make_database(std::string_view dsn);

void init_database();
odb::database &database();
void reset_stores_for_test();
void reset_database_for_test();

// Begins a transaction only when the calling thread does not already have one.
// Nested Store calls can share the outer transaction safely.
class ScopedTransaction {
public:
    explicit ScopedTransaction(odb::database &db)
    {
        if (!odb::transaction::has_current()) {
            txn_.emplace(db.begin());
        }
    }

    ScopedTransaction(const ScopedTransaction &) = delete;
    ScopedTransaction &operator=(const ScopedTransaction &) = delete;

    void commit()
    {
        if (txn_) {
            txn_->commit();
            txn_.reset();
        }
    }

private:
    std::optional<odb::transaction> txn_;
};

using SqlResultRow = std::vector<std::optional<std::string>>;

void sql_exec(odb::database &db, std::string_view sql);
std::optional<std::string> sql_query_one(odb::database &db, std::string_view sql);
std::vector<SqlResultRow> sql_query_rows(odb::database &db, std::string_view sql);
std::string sql_quote(odb::database &db, std::string_view value);

inline std::string sql_nullable(odb::database &db, const std::optional<std::string> &value)
{
    return value.has_value() ? sql_quote(db, *value) : "NULL";
}

inline std::string sql_nullable(odb::database &db, const odb::nullable<std::string> &value)
{
    return value.null() ? std::string("NULL") : sql_quote(db, *value);
}

} // namespace revlm
