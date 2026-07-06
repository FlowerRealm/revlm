#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace soci
{
class session;
class transaction;
class soci_error;
} // namespace soci

namespace revlm
{

constexpr unsigned long mysql_client_multi_statements = 1UL << 16;

struct ParsedMysqlDsn {
    std::string user;
    std::string password;
    std::string host = "127.0.0.1";
    unsigned int port = 3306;
    std::string database;
};

using MysqlResultRow = std::vector<std::optional<std::string>>;

ParsedMysqlDsn parse_mysql_dsn(std::string_view raw);
std::string build_soci_mysql_connstr(const ParsedMysqlDsn &dsn, unsigned long client_flags = 0);
std::string sql_quote(std::string_view value);

class MysqlConnection {
public:
    explicit MysqlConnection(const ParsedMysqlDsn &dsn, unsigned long client_flags = 0);
    explicit MysqlConnection(std::string_view dsn, unsigned long client_flags = 0);

    MysqlConnection(const MysqlConnection &) = delete;
    MysqlConnection &operator=(const MysqlConnection &) = delete;

    ~MysqlConnection();

    void exec(std::string_view sql);
    std::optional<std::string> query_one(std::string_view sql);
    std::vector<MysqlResultRow> query_rows(std::string_view sql);
    std::string quote(std::string_view value) const;
    unsigned long long last_insert_id() const;
    unsigned long long affected_rows() const;
    unsigned int last_errno() const;
    std::string error() const;

    soci::session &session();
    const soci::session &session() const;

private:
    void note_error(const soci::soci_error &err);

    void exec_one(std::string_view sql);
    void exec_statements(std::string_view sql);

    std::unique_ptr<soci::session> sql_;
    unsigned long client_flags_ = 0;
    unsigned long long last_exec_affected_rows_ = 0;
    unsigned int last_errno_ = 0;
    std::string last_error_;
};

class DbTransaction {
public:
    explicit DbTransaction(MysqlConnection &conn);
    ~DbTransaction();

    DbTransaction(const DbTransaction &) = delete;
    DbTransaction &operator=(const DbTransaction &) = delete;

    void commit();

private:
    MysqlConnection &conn_;
    std::unique_ptr<soci::transaction> tr_;
    bool committed_ = false;
};

inline std::string sql_nullable(MysqlConnection &conn, const std::optional<std::string> &value)
{
    return value.has_value() ? conn.quote(*value) : "NULL";
}

inline std::string sql_nullable_bool(const std::optional<bool> &value)
{
    if (!value.has_value()) {
        return "NULL";
    }
    return *value ? "1" : "0";
}

inline std::string sql_nullable_int(const std::optional<int> &value)
{
    return value.has_value() ? std::to_string(*value) : "NULL";
}

inline std::string sql_nullable_i64(const std::optional<long long> &value)
{
    return value.has_value() ? std::to_string(*value) : "NULL";
}

} // namespace revlm
