#include "auth/crypto.hpp"
#include "auth/users.hpp"

#include "util/strings.hpp"
#include "util/user_input.hpp"

#include <crypt.h>

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace revlm
{
namespace
{

std::string bcrypt_salt()
{
    static constexpr char alphabet[] = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    const std::string raw = random_bytes(16);
    std::string salt = "$2b$12$";
    int bits = 0;
    unsigned int acc = 0;
    for (unsigned char ch : raw) {
        acc |= static_cast<unsigned int>(ch) << bits;
        bits += 8;
        while (bits >= 6 && salt.size() < 29) {
            salt.push_back(alphabet[acc & 0x3f]);
            acc >>= 6;
            bits -= 6;
        }
    }
    if (salt.size() < 29) {
        salt.push_back(alphabet[acc & 0x3f]);
    }
    salt.resize(29, '.');
    return salt;
}

std::optional<User> row_to_user(const MysqlResultRow &row)
{
    if (row.size() < 7 || !row[0].has_value()) {
        return std::nullopt;
    }
    return User(std::stoll(*row[0]), row[1].value_or(""), row[2].value_or(""), row[3].value_or(""), row[4].value_or(""),
                std::stoi(row[5].value_or("0")), row[6].value_or(""));
}

std::string format_timestamp_minute(std::string_view raw)
{
    std::string text = trim_ascii(raw);
    if (text.size() >= 16) {
        return text.substr(0, 16);
    }
    return text;
}

long long exec_row_count(MysqlConnection &conn, const std::string &sql)
{
    conn.exec(sql);
    return std::stoll(conn.query_one("SELECT ROW_COUNT()").value_or("-1"));
}

void quoted_positive_user_id(long long user_id, MysqlConnection &conn)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    if (user_id <= 0) {
        return;
    }
    if (!conn.query_one("SELECT id FROM users WHERE id=" + std::to_string(user_id)).has_value()) {
        throw std::runtime_error("用户不存在");
    }
}

bool mysql_table_exists(MysqlConnection &conn, std::string_view table)
{
    return conn.query_one("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
                          "AND table_name=" +
                          conn.quote(table))
               .value_or("0") != "0";
}

std::optional<User> row_to_admin_list_user(const MysqlResultRow &row)
{
    if (row.size() < 7 || !row[0].has_value()) {
        return std::nullopt;
    }
    User user;
    user.id = std::stoll(*row[0]);
    user.email = row[1].value_or("");
    user.username = row[2].value_or("");
    user.role = row[3].value_or("");
    user.status = std::stoi(row[4].value_or("0"));
    user.balance_usd = format_usd_plain_fixed6(row[5].value_or("0"));
    user.created_at = format_timestamp_minute(row[6].value_or(""));
    return user;
}

} // namespace

std::string hash_password(std::string_view password)
{
    require_password_length(password);
    const std::string salt = bcrypt_salt();
    crypt_data data{};
    data.initialized = 0;
    char *hash = ::crypt_r(std::string{ password }.c_str(), salt.c_str(), &data);
    if (hash == nullptr || std::strncmp(hash, "$2", 2) != 0) {
        throw std::runtime_error("密码哈希失败");
    }
    return std::string{ hash };
}

bool check_password(std::string_view hash, std::string_view password)
{
    if (!hash.starts_with("$2")) {
        return false;
    }
    crypt_data data{};
    data.initialized = 0;
    char *got = ::crypt_r(std::string{ password }.c_str(), std::string{ hash }.c_str(), &data);
    if (got == nullptr) {
        return false;
    }
    const std::string got_text{ got };
    return constant_time_equal(got_text, hash);
}

UserStore::UserStore(MysqlConnection &conn)
    : conn_(conn)
{
}

long long UserStore::count_users()
{
    return std::stoll(conn_.query_one("SELECT COUNT(*) FROM users").value_or("0"));
}

long long UserStore::create_user(const User &user)
{
    const std::string sql = "INSERT INTO users(email, username, password_hash, role, status, created_at) VALUES(" +
                            conn_.quote(user.email) + ", " + conn_.quote(user.username) + ", " +
                            conn_.quote(user.password_hash) + ", " + conn_.quote(user.role) + ", 1, CURRENT_TIMESTAMP)";
    conn_.exec(sql);
    return static_cast<long long>(conn_.last_insert_id());
}

std::optional<User> UserStore::get_user_by_id(long long id)
{
    assert(id > 0 && "internal: user id must be positive");
    if (id <= 0) {
        return std::nullopt;
    }
    return get_user_by_sql("SELECT id,email,username,password_hash,role,status,created_at FROM users WHERE id=" +
                           std::to_string(id));
}

std::optional<User> UserStore::get_user_by_id_for_update(long long id)
{
    assert(id > 0 && "internal: user id must be positive");
    if (id <= 0) {
        return std::nullopt;
    }
    return get_user_by_sql("SELECT id,email,username,password_hash,role,status,created_at FROM users WHERE id=" +
                           std::to_string(id) + " FOR UPDATE");
}

std::optional<User> UserStore::get_user_by_email(std::string_view email)
{
    return get_user_by_sql("SELECT id,email,username,password_hash,role,status,created_at FROM users WHERE email=" +
                           conn_.quote(email));
}

std::optional<User> UserStore::get_user_by_username(std::string_view username)
{
    return get_user_by_sql("SELECT id,email,username,password_hash,role,status,created_at FROM users WHERE username=" +
                           conn_.quote(username));
}

std::vector<User> UserStore::list_admin_users()
{
    const std::string sql = "SELECT u.id,u.email,u.username,u.role,u.status,COALESCE(b.usd,0),u.created_at "
                            "FROM users u "
                            "LEFT JOIN user_balances b ON b.user_id=u.id "
                            "ORDER BY u.id DESC";
    const auto rows = conn_.query_rows(sql);
    std::vector<User> users;
    users.reserve(rows.size());
    for (const auto &row : rows) {
        if (auto user = row_to_admin_list_user(row); user.has_value()) {
            users.push_back(std::move(*user));
        }
    }
    return users;
}

void UserStore::update_user_email(long long user_id, std::string_view email)
{
    assert(user_id > 0 && "internal: user id must be positive");
    if (user_id <= 0) {
        return;
    }
    conn_.exec("UPDATE users SET email=" + conn_.quote(email) + " WHERE id=" + std::to_string(user_id));
    if (conn_.affected_rows() > 1) {
        throw std::runtime_error("update user email affected unexpected row count");
    }
}

void UserStore::update_user_password_hash(long long user_id, std::string_view password_hash)
{
    assert(user_id > 0 && "internal: user id must be positive");
    if (user_id <= 0) {
        return;
    }
    conn_.exec("UPDATE users SET password_hash=" + conn_.quote(password_hash) + " WHERE id=" + std::to_string(user_id));
    if (conn_.affected_rows() != 1) {
        throw std::runtime_error("update user password affected unexpected row count");
    }
}

void UserStore::set_user_role(long long user_id, std::string_view role)
{
    quoted_positive_user_id(user_id, conn_);
    conn_.exec("UPDATE users SET role=" + conn_.quote(role) + " WHERE id=" + std::to_string(user_id));
    if (conn_.affected_rows() > 1) {
        throw std::runtime_error("update user role affected unexpected row count");
    }
}

void UserStore::set_user_status(long long user_id, int status)
{
    quoted_positive_user_id(user_id, conn_);
    conn_.exec("UPDATE users SET status=" + std::to_string(status) + " WHERE id=" + std::to_string(user_id));
    if (conn_.affected_rows() > 1) {
        throw std::runtime_error("update user status affected unexpected row count");
    }
}

std::optional<std::string> UserStore::add_user_balance_usd(long long user_id, std::string_view amount_usd)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    if (user_id <= 0) {
        return std::nullopt;
    }
    const std::string amount = normalize_usd_amount(amount_usd);
    DbTransaction tr(conn_);
    const std::string user_id_sql = std::to_string(user_id);
    if (!conn_.query_one("SELECT id FROM users WHERE id=" + user_id_sql + " FOR UPDATE").has_value()) {
        return std::nullopt;
    }
    conn_.exec("INSERT INTO user_balances(user_id, usd, created_at, updated_at) VALUES(" + user_id_sql + ", " + amount +
               ", CURRENT_TIMESTAMP, CURRENT_TIMESTAMP) "
               "ON DUPLICATE KEY UPDATE usd=usd+VALUES(usd), updated_at=CURRENT_TIMESTAMP");
    const std::string balance = format_usd_plain_fixed6(
        conn_.query_one("SELECT usd FROM user_balances WHERE user_id=" + user_id_sql).value_or("0"));
    tr.commit();
    return balance;
}

bool UserStore::delete_user(long long user_id)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    if (user_id <= 0) {
        return false;
    }
    DbTransaction tr(conn_);
    const std::string user_id_sql = std::to_string(user_id);
    if (!conn_.query_one("SELECT id FROM users WHERE id=" + user_id_sql + " FOR UPDATE").has_value()) {
        return false;
    }
    if (mysql_table_exists(conn_, "subscription_orders")) {
        conn_.exec("DELETE FROM subscription_orders WHERE user_id=" + user_id_sql);
    }
    if (mysql_table_exists(conn_, "user_subscriptions")) {
        conn_.exec("DELETE FROM user_subscriptions WHERE user_id=" + user_id_sql);
    }
    conn_.exec("DELETE FROM usage_events WHERE user_id=" + user_id_sql +
               " OR token_id IN (SELECT id FROM user_tokens WHERE user_id=" + user_id_sql + ")");
    if (mysql_table_exists(conn_, "topup_orders")) {
        conn_.exec("DELETE FROM topup_orders WHERE user_id=" + user_id_sql);
    }
    conn_.exec("DELETE FROM user_balances WHERE user_id=" + user_id_sql);
    conn_.exec("DELETE FROM token_model_mappings WHERE token_id IN "
               "(SELECT id FROM user_tokens WHERE user_id=" +
               user_id_sql + ")");
    conn_.exec("DELETE FROM token_channel_groups WHERE token_id IN "
               "(SELECT id FROM user_tokens WHERE user_id=" +
               user_id_sql + ")");
    conn_.exec("DELETE FROM session_bindings WHERE user_id=" + user_id_sql);
    conn_.exec("DELETE FROM user_tokens WHERE user_id=" + user_id_sql);
    if (exec_row_count(conn_, "DELETE FROM users WHERE id=" + user_id_sql) != 1) {
        return false;
    }
    tr.commit();
    return true;
}

std::optional<User> UserStore::get_user_by_sql(const std::string &sql)
{
    const auto rows = conn_.query_rows(sql);
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_user(rows[0]);
}

} // namespace revlm
