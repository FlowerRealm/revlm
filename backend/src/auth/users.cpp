#include "auth/users.hpp"

#include "util/user_input.hpp"

#include <string>
#include <string_view>

namespace revlm
{
namespace
{

User *find_user(std::vector<User> &users, long long id)
{
    for (User &user : users)
        if (user.id == id)
            return &user;
    return nullptr;
}

} // namespace

UserStore &UserStore::instance()
{
    static UserStore store;
    return store;
}

void UserStore::reload(MysqlConnection &conn)
{
    conn_ = &conn;
    load_from_db(conn);
    token_store_.reload(conn);
}

void UserStore::load_from_db(MysqlConnection &conn)
{
    users_.clear();
    next_id_ = 0;
    if (auto max_id = conn.query_one("SELECT COALESCE(MAX(id),0) FROM users"))
        next_id_ = std::stoll(*max_id);
    for (const MysqlResultRow &row :
         conn.query_rows("SELECT id,email,username,password_hash,role,status FROM users ORDER BY id"))
        users_.push_back(User(std::stoll(row[0].value_or("0")), row[1].value_or(""), row[2].value_or(""),
                              row[3].value_or(""), row[4].value_or(""), std::stoi(row[5].value_or("0"))));
}

long long UserStore::count_users()
{
    return static_cast<long long>(users_.size());
}

long long UserStore::create_user(User user)
{
    user.id = ++next_id_;
    users_.push_back(std::move(user));
    align_user_to_db(users_.back().id);
    return users_.back().id;
}

User UserStore::get_user_by_id(long long id)
{
    if (User *user = find_user(users_, id))
        return *user;
    return {};
}

User UserStore::get_user_by_email(std::string_view email)
{
    for (const User &user : users_)
        if (user.email == email)
            return user;
    return {};
}

User UserStore::get_user_by_username(std::string_view username)
{
    for (const User &user : users_)
        if (user.username == username)
            return user;
    return {};
}

std::vector<User> UserStore::list_users()
{
    return users_;
}

bool UserStore::update_user(const User &user)
{
    User *existing = find_user(users_, user.id);
    if (existing == nullptr)
        return false;
    *existing = user;
    align_user_to_db(existing->id);
    // balance_usd on incoming is a credit delta for this call only; identity cache stays empty.
    existing->balance_usd.clear();
    return true;
}

bool UserStore::delete_user(long long user_id)
{
    if (find_user(users_, user_id) == nullptr)
        return false;
    std::erase_if(users_, [user_id](const User &user) { return user.id == user_id; });
    return align_user_to_db(user_id);
}

bool UserStore::align_user_to_db(long long id)
{
    User *user = find_user(users_, id);
    if (user == nullptr) {
        DbTransaction tr(*conn_);
        const std::string user_id_sql = std::to_string(id);
        conn_->exec("DELETE FROM requests WHERE user_id=" + user_id_sql +
                    " OR token_id IN (SELECT id FROM user_tokens WHERE user_id=" + user_id_sql + ")");
        conn_->exec("DELETE FROM request_totals WHERE user_id=" + user_id_sql +
                    " OR token_id IN (SELECT id FROM user_tokens WHERE user_id=" + user_id_sql + ")");
        conn_->exec("DELETE FROM user_balances WHERE user_id=" + user_id_sql);
        conn_->exec("DELETE FROM token_model_mappings WHERE token_id IN "
                    "(SELECT id FROM user_tokens WHERE user_id=" +
                    user_id_sql + ")");
        conn_->exec("DELETE FROM token_channel_groups WHERE token_id IN "
                    "(SELECT id FROM user_tokens WHERE user_id=" +
                    user_id_sql + ")");
        conn_->exec("DELETE FROM session_bindings WHERE user_id=" + user_id_sql);
        conn_->exec("DELETE FROM user_tokens WHERE user_id=" + user_id_sql);
        conn_->exec("DELETE FROM users WHERE id=" + user_id_sql);
        tr.commit();
        return true;
    }

    DbTransaction tr(*conn_);
    const std::string id_sql = std::to_string(user->id);
    conn_->exec("UPDATE users SET email=" + conn_->quote(user->email) + ",username=" + conn_->quote(user->username) +
                ",password_hash=" + conn_->quote(user->password_hash) + ",role=" + conn_->quote(user->role) +
                ",status=" + std::to_string(user->status) + " WHERE id=" + id_sql);
    if (conn_->affected_rows() == 0)
        conn_->exec("INSERT INTO users(id,email,username,password_hash,role,status) VALUES(" + id_sql + "," +
                    conn_->quote(user->email) + "," + conn_->quote(user->username) + "," +
                    conn_->quote(user->password_hash) + "," + conn_->quote(user->role) + "," +
                    std::to_string(user->status) + ")");

    if (!user->balance_usd.empty()) {
        const std::string amount = normalize_usd_amount(user->balance_usd);
        conn_->exec("INSERT INTO user_balances(user_id, usd, created_at, updated_at) VALUES(" + id_sql + ", " + amount +
                    ", CURRENT_TIMESTAMP, CURRENT_TIMESTAMP) "
                    "ON DUPLICATE KEY UPDATE usd=usd+VALUES(usd), updated_at=CURRENT_TIMESTAMP");
    }

    tr.commit();
    return true;
}

} // namespace revlm
