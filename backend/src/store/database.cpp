#include "store/database.hpp"

#include <charconv>
#include <stdexcept>
#include <string>

#include <odb/mysql/database.hxx>
#include <odb/mysql/connection.hxx>
#include <odb/schema-catalog.hxx>
#include <odb/transaction.hxx>

#include <mysql/mysql.h>

namespace revlm
{
namespace
{

unsigned int parse_port(std::string_view raw)
{
    unsigned int port = 0;
    const auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), port);
    if (ec != std::errc{} || ptr != raw.data() + raw.size() || port == 0 || port > 65535) {
        throw std::invalid_argument("REVLM_DB_DSN tcp port is invalid");
    }
    return port;
}

} // namespace

ParsedMysqlDsn parse_mysql_dsn(std::string_view raw)
{
    const size_t at = raw.find('@');
    if (at == std::string_view::npos) {
        throw std::invalid_argument("REVLM_DB_DSN must use user:pass@tcp(host:port)/db");
    }
    const std::string_view user_pass = raw.substr(0, at);
    const size_t colon = user_pass.find(':');
    if (colon == std::string_view::npos) {
        throw std::invalid_argument("REVLM_DB_DSN must include password");
    }

    ParsedMysqlDsn out;
    out.user = std::string{ user_pass.substr(0, colon) };
    out.password = std::string{ user_pass.substr(colon + 1) };
    if (out.user.empty()) {
        throw std::invalid_argument("REVLM_DB_DSN user must not be empty");
    }

    std::string_view rest = raw.substr(at + 1);
    constexpr std::string_view tcp_prefix = "tcp(";
    if (!rest.starts_with(tcp_prefix)) {
        throw std::invalid_argument("REVLM_DB_DSN must use tcp(host:port)");
    }
    rest.remove_prefix(tcp_prefix.size());
    const size_t close = rest.find(')');
    if (close == std::string_view::npos) {
        throw std::invalid_argument("REVLM_DB_DSN tcp address is invalid");
    }
    const std::string_view address = rest.substr(0, close);
    rest.remove_prefix(close + 1);
    if (!rest.starts_with('/')) {
        throw std::invalid_argument("REVLM_DB_DSN database is missing");
    }
    rest.remove_prefix(1);
    const size_t query = rest.find('?');
    out.database = std::string{ query == std::string_view::npos ? rest : rest.substr(0, query) };
    if (out.database.empty()) {
        throw std::invalid_argument("REVLM_DB_DSN database must not be empty");
    }

    const size_t host_colon = address.rfind(':');
    if (host_colon == std::string_view::npos) {
        out.host = std::string{ address };
    } else {
        out.host = std::string{ address.substr(0, host_colon) };
        out.port = parse_port(address.substr(host_colon + 1));
    }
    if (out.host.empty()) {
        out.host = "127.0.0.1";
    }
    return out;
}

std::unique_ptr<odb::database> make_database(const ParsedMysqlDsn &dsn)
{
    return std::make_unique<odb::mysql::database>(dsn.user, dsn.password, dsn.database, dsn.host, dsn.port,
                                                  "", "", CLIENT_MULTI_STATEMENTS);
}

std::unique_ptr<odb::database> make_database(std::string_view dsn)
{
    return make_database(parse_mysql_dsn(dsn));
}

void ensure_schema(odb::database &db)
{
    // Idempotent: embedded create_schema fails if any table already exists.
    // Native SQL helpers require an active ODB transaction (connection()).
    {
        odb::transaction probe(db.begin());
        const bool exists =
            sql_query_one(db,
                          "SELECT 1 FROM information_schema.tables "
                          "WHERE table_schema = DATABASE() AND table_name = 'users' "
                          "LIMIT 1")
                .value_or("") == "1";
        probe.commit();
        if (exists) {
            return;
        }
    }

    odb::transaction t(db.begin());
    odb::schema_catalog::create_schema(db, "", false);
    t.commit();
}

void sql_exec(odb::database &db, std::string_view sql)
{
    odb::connection_ptr c(db.connection());
    MYSQL *h = static_cast<odb::mysql::connection &>(*c).handle();
    if (mysql_real_query(h, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        throw std::runtime_error(std::string("sql_exec: ") + mysql_error(h));
    }
    do {
        MYSQL_RES *res = mysql_store_result(h);
        if (res) {
            mysql_free_result(res);
        }
    } while (mysql_next_result(h) == 0);
}

std::optional<std::string> sql_query_one(odb::database &db, std::string_view sql)
{
    odb::connection_ptr c(db.connection());
    MYSQL *h = static_cast<odb::mysql::connection &>(*c).handle();
    if (mysql_real_query(h, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        throw std::runtime_error(std::string("sql_query_one: ") + mysql_error(h));
    }
    MYSQL_RES *res = mysql_store_result(h);
    if (!res) {
        return std::nullopt;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    std::optional<std::string> result;
    if (row && row[0]) {
        unsigned long *lengths = mysql_fetch_lengths(res);
        result.emplace(row[0], lengths[0]);
    }
    mysql_free_result(res);
    return result;
}

std::vector<SqlResultRow> sql_query_rows(odb::database &db, std::string_view sql)
{
    odb::connection_ptr c(db.connection());
    MYSQL *h = static_cast<odb::mysql::connection &>(*c).handle();
    if (mysql_real_query(h, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        throw std::runtime_error(std::string("sql_query_rows: ") + mysql_error(h));
    }
    MYSQL_RES *res = mysql_store_result(h);
    if (!res) {
        return {};
    }
    const unsigned int num_fields = mysql_num_fields(res);
    std::vector<SqlResultRow> out;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr) {
        unsigned long *lengths = mysql_fetch_lengths(res);
        SqlResultRow r;
        r.reserve(num_fields);
        for (unsigned int i = 0; i < num_fields; ++i) {
            if (row[i]) {
                r.emplace_back(std::string(row[i], lengths[i]));
            } else {
                r.emplace_back(std::nullopt);
            }
        }
        out.push_back(std::move(r));
    }
    mysql_free_result(res);
    return out;
}

std::string sql_quote(odb::database &db, std::string_view value)
{
    odb::connection_ptr c(db.connection());
    MYSQL *h = static_cast<odb::mysql::connection &>(*c).handle();
    std::string buf(value.size() * 2 + 3, '\0');
    buf[0] = '\'';
    unsigned long len =
        mysql_real_escape_string(h, buf.data() + 1, value.data(), static_cast<unsigned long>(value.size()));
    buf[1 + len] = '\'';
    buf.resize(2 + len);
    return buf;
}

} // namespace revlm
