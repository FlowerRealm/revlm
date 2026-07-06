#include "store/mysql.hpp"

#include <soci/mysql/soci-mysql.h>
#include <soci/soci.h>

#include <charconv>
#include <sstream>
#include <stdexcept>
#include <string>
#include "util/strings.hpp"

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

std::string row_value_as_string(const soci::row &row, std::size_t index)
{
    switch (row.get_properties(index).get_data_type()) {
    case soci::dt_string:
        return row.get<std::string>(index);
    case soci::dt_integer:
        return std::to_string(row.get<int>(index));
    case soci::dt_long_long:
        return std::to_string(row.get<long long>(index));
    case soci::dt_unsigned_long_long:
        return std::to_string(row.get<unsigned long long>(index));
    case soci::dt_double:
        return std::to_string(row.get<double>(index));
    default:
        break;
    }
    return row.get<std::string>(index);
}

MysqlResultRow row_to_result(const soci::row &row)
{
    MysqlResultRow out;
    out.reserve(row.size());
    for (std::size_t i = 0; i < row.size(); ++i) {
        if (row.get_indicator(i) == soci::i_null) {
            out.push_back(std::nullopt);
            continue;
        }
        out.push_back(row_value_as_string(row, i));
    }
    return out;
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

std::string build_soci_mysql_connstr(const ParsedMysqlDsn &dsn, unsigned long client_flags)
{
    std::ostringstream out;
    out << "db=" << dsn.database << " user=" << dsn.user << " password=" << dsn.password << " host=" << dsn.host
        << " port=" << dsn.port;
    if ((client_flags & mysql_client_multi_statements) != 0) {
        out << " clientFlags=" << mysql_client_multi_statements;
    }
    return out.str();
}

std::string sql_quote(std::string_view value)
{
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

MysqlConnection::MysqlConnection(const ParsedMysqlDsn &dsn, unsigned long client_flags)
    : client_flags_(client_flags)
{
    try {
        sql_ = std::make_unique<soci::session>(soci::mysql, build_soci_mysql_connstr(dsn, client_flags));
        last_errno_ = 0;
        last_error_.clear();
    } catch (const soci::soci_error &err) {
        note_error(err);
        throw std::runtime_error("connect MySQL: " + last_error_);
    }
}

MysqlConnection::MysqlConnection(std::string_view dsn, unsigned long client_flags)
    : MysqlConnection(parse_mysql_dsn(dsn), client_flags)
{
}

MysqlConnection::~MysqlConnection() = default;

soci::session &MysqlConnection::session()
{
    return *sql_;
}

const soci::session &MysqlConnection::session() const
{
    return *sql_;
}

void MysqlConnection::note_error(const soci::soci_error &err)
{
    if (const auto *mysql_err = dynamic_cast<const soci::mysql_soci_error *>(&err)) {
        last_errno_ = mysql_err->err_num_;
    } else {
        last_errno_ = 0;
    }
    last_error_ = err.what();
}

void MysqlConnection::exec_one(std::string_view sql)
{
    try {
        soci::statement st = (sql_->prepare << std::string{ sql });
        st.execute(true);
        last_exec_affected_rows_ = static_cast<unsigned long long>(st.get_affected_rows());
        last_errno_ = 0;
        last_error_.clear();
    } catch (const soci::soci_error &err) {
        note_error(err);
        throw std::runtime_error(last_error_);
    }
}

void MysqlConnection::exec_statements(std::string_view sql)
{
    std::string_view rest = sql;
    while (!rest.empty()) {
        const size_t semi = rest.find(';');
        if (semi == std::string_view::npos) {
            const std::string stmt = trim_ascii(rest);
            if (!stmt.empty()) {
                exec_one(stmt);
            }
            return;
        }
        const std::string stmt = trim_ascii(rest.substr(0, semi));
        if (!stmt.empty()) {
            exec_one(stmt);
        }
        rest.remove_prefix(semi + 1);
    }
}

void MysqlConnection::exec(std::string_view sql)
{
    if ((client_flags_ & mysql_client_multi_statements) != 0 && sql.find(';') != std::string_view::npos) {
        exec_statements(sql);
        return;
    }
    exec_one(sql);
}

std::optional<std::string> MysqlConnection::query_one(std::string_view sql)
{
    const std::vector<MysqlResultRow> rows = query_rows(sql);
    if (rows.empty() || rows[0].empty()) {
        return std::nullopt;
    }
    return rows[0][0];
}

std::vector<MysqlResultRow> MysqlConnection::query_rows(std::string_view sql)
{
    std::vector<MysqlResultRow> rows;
    try {
        soci::rowset<soci::row> rs = (sql_->prepare << std::string{ sql });
        for (const soci::row &row : rs) {
            rows.push_back(row_to_result(row));
        }
        last_errno_ = 0;
        last_error_.clear();
    } catch (const soci::soci_error &err) {
        note_error(err);
        throw std::runtime_error(last_error_);
    }
    return rows;
}

std::string MysqlConnection::quote(std::string_view value) const
{
    soci::mysql_session_backend *backend =
        dynamic_cast<soci::mysql_session_backend *>(const_cast<MysqlConnection *>(this)->sql_->get_backend());
    if (backend == nullptr || backend->conn_ == nullptr) {
        return sql_quote(value);
    }
    std::string escaped(value.size() * 2 + 1, '\0');
    const auto n = mysql_real_escape_string(backend->conn_, escaped.data(), value.data(),
                                            static_cast<unsigned long>(value.size()));
    escaped.resize(n);
    return "'" + escaped + "'";
}

unsigned long long MysqlConnection::last_insert_id() const
{
    long long id = 0;
    if (!sql_->get_last_insert_id(std::string{}, id)) {
        return 0;
    }
    return static_cast<unsigned long long>(id);
}

unsigned long long MysqlConnection::affected_rows() const
{
    return last_exec_affected_rows_;
}

unsigned int MysqlConnection::last_errno() const
{
    return last_errno_;
}

std::string MysqlConnection::error() const
{
    return last_error_;
}

DbTransaction::DbTransaction(MysqlConnection &conn)
    : conn_(conn)
{
    tr_ = std::make_unique<soci::transaction>(conn.session());
}

DbTransaction::~DbTransaction()
{
    if (!committed_ && tr_ != nullptr) {
        try {
            tr_->rollback();
        } catch (const soci::soci_error &) {
        }
    }
}

void DbTransaction::commit()
{
    if (tr_ == nullptr || committed_) {
        return;
    }
    tr_->commit();
    committed_ = true;
}

} // namespace revlm
