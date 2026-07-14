#include "auth/users.hpp"

#include "store/database.hpp"
#include "revlm_entities-odb.hxx"
#include "util/user_input.hpp"

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <string>
#include <string_view>

namespace revlm
{

UserStore::UserStore(odb::database &db)
    : db_(db)
    , tokens_(db)
{
}

TokenStore &UserStore::tokens()
{
    return tokens_;
}

long long UserStore::count_users()
{
    ScopedTransaction t(db_);
    const auto v = sql_query_one(db_, "SELECT COUNT(*) FROM users");
    t.commit();
    return v ? std::stoll(*v) : 0;
}

long long UserStore::create_user(User user)
{
    ScopedTransaction t(db_);
    db_.persist(user);
    t.commit();
    return user.id;
}

User UserStore::get_user_by_id(long long id)
{
    ScopedTransaction t(db_);
    auto p = db_.find<User>(id);
    t.commit();
    return p ? *p : User{};
}

User UserStore::get_user_by_email(std::string_view email)
{
    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(db_, "SELECT id,email,username,password_hash,role,status FROM users WHERE email=" +
                                              sql_quote(db_, email) + " LIMIT 1");
    t.commit();
    if (rows.empty()) {
        return {};
    }
    const auto &row = rows.front();
    return User(std::stoll(row[0].value_or("0")), row[1].value_or(""), row[2].value_or(""), row[3].value_or(""),
                row[4].value_or(""), std::stoi(row[5].value_or("0")));
}

User UserStore::get_user_by_username(std::string_view username)
{
    ScopedTransaction t(db_);
    const auto rows =
        sql_query_rows(db_, "SELECT id,email,username,password_hash,role,status FROM users WHERE username=" +
                                sql_quote(db_, username) + " LIMIT 1");
    t.commit();
    if (rows.empty()) {
        return {};
    }
    const auto &row = rows.front();
    return User(std::stoll(row[0].value_or("0")), row[1].value_or(""), row[2].value_or(""), row[3].value_or(""),
                row[4].value_or(""), std::stoi(row[5].value_or("0")));
}

std::vector<User> UserStore::list_users()
{
    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(db_, "SELECT id,email,username,password_hash,role,status FROM users ORDER BY id");
    t.commit();
    std::vector<User> out;
    for (const auto &row : rows) {
        out.push_back(User(std::stoll(row[0].value_or("0")), row[1].value_or(""), row[2].value_or(""),
                           row[3].value_or(""), row[4].value_or(""), std::stoi(row[5].value_or("0"))));
    }
    return out;
}

bool UserStore::update_user(const User &user)
{
    ScopedTransaction t(db_);

    const auto exists = sql_query_one(db_, "SELECT id FROM users WHERE id=" + std::to_string(user.id) + " LIMIT 1");
    if (!exists.has_value()) {
        return false;
    }

    sql_exec(db_, "UPDATE users SET email=" + sql_quote(db_, user.email) +
                      ",username=" + sql_quote(db_, user.username) +
                      ",password_hash=" + sql_quote(db_, user.password_hash) + ",role=" + sql_quote(db_, user.role) +
                      ",status=" + std::to_string(user.status) + " WHERE id=" + std::to_string(user.id));

    if (!user.balance_usd.empty()) {
        const std::string amount = normalize_usd_amount(user.balance_usd);
        sql_exec(db_, "INSERT INTO user_balances(user_id, usd) VALUES(" + std::to_string(user.id) + ", " + amount +
                          ") ON DUPLICATE KEY UPDATE usd=usd+VALUES(usd)");
    }

    t.commit();
    return true;
}

bool UserStore::delete_user(long long user_id)
{
    ScopedTransaction t(db_);
    const auto exists = sql_query_one(db_, "SELECT id FROM users WHERE id=" + std::to_string(user_id) + " LIMIT 1");
    if (!exists.has_value()) {
        return false;
    }

    const std::string uid = std::to_string(user_id);
    sql_exec(db_, "DELETE FROM requests WHERE user_id=" + uid +
                      " OR token_id IN (SELECT id FROM user_tokens WHERE user_id=" + uid + ")");
    sql_exec(db_, "DELETE FROM request_totals WHERE user_id=" + uid +
                      " OR token_id IN (SELECT id FROM user_tokens WHERE user_id=" + uid + ")");
    sql_exec(db_, "DELETE FROM user_balances WHERE user_id=" + uid);
    sql_exec(db_, "DELETE FROM token_model_mappings WHERE token_id IN "
                  "(SELECT id FROM user_tokens WHERE user_id=" +
                      uid + ")");
    sql_exec(db_, "DELETE FROM token_channel_groups WHERE token_id IN "
                  "(SELECT id FROM user_tokens WHERE user_id=" +
                      uid + ")");
    sql_exec(db_, "DELETE FROM session_bindings WHERE user_id=" + uid);
    sql_exec(db_, "DELETE FROM user_tokens WHERE user_id=" + uid);
    sql_exec(db_, "DELETE FROM users WHERE id=" + uid);
    t.commit();
    return true;
}

} // namespace revlm
