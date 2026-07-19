#include "users/users.hpp"

#include "auth/session.hpp"
#include "store/database.hpp"
#include "revlm_entities-odb.hxx"

#include <odb/database.hxx>
#include <odb/query.hxx>
#include <odb/transaction.hxx>

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace revlm
{
namespace
{

std::unique_ptr<UserStore> g_user_store;

} // namespace

UserStore &UserStore::instance()
{
    if (!g_user_store) {
        g_user_store.reset(new UserStore());
    }
    return *g_user_store;
}

void UserStore::reset_instance()
{
    g_user_store.reset();
}

UserStore::UserStore()
    : db_(database())
    , tokens_()
{
}

TokenStore &UserStore::tokens()
{
    return tokens_;
}

long long UserStore::count_users()
{
    ScopedTransaction t(db_);
    auto r = db_.query<User>();
    r.cache();
    const long long n = static_cast<long long>(r.size());
    t.commit();
    return n;
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
    using query = odb::query<User>;
    ScopedTransaction t(db_);
    auto r = db_.query<User>(query::email == std::string(email));
    auto i = r.begin();
    User out = i != r.end() ? *i : User{};
    t.commit();
    return out;
}

User UserStore::get_user_by_username(std::string_view username)
{
    using query = odb::query<User>;
    ScopedTransaction t(db_);
    auto r = db_.query<User>(query::username == std::string(username));
    auto i = r.begin();
    User out = i != r.end() ? *i : User{};
    t.commit();
    return out;
}

std::vector<User> UserStore::list_users()
{
    using query = odb::query<User>;
    ScopedTransaction t(db_);
    std::vector<User> out;
    for (const User &u : db_.query<User>("ORDER BY" + query::id)) {
        out.push_back(u);
    }
    t.commit();
    return out;
}

bool UserStore::update_user(const User &user)
{
    ScopedTransaction t(db_);
    auto p = db_.find<User>(user.id);
    if (!p) {
        return false;
    }

    p->email = user.email;
    p->username = user.username;
    p->password_hash = user.password_hash;
    p->role = user.role;
    p->status = user.status;
    p->balance_usd = user.balance_usd;
    db_.update(*p);
    t.commit();
    return true;
}

bool UserStore::delete_user(long long user_id)
{
    ScopedTransaction t(db_);
    auto p = db_.find<User>(user_id);
    if (!p) {
        return false;
    }

    for (const UserToken &tok : tokens_.list_user_tokens(user_id)) {
        tokens_.delete_user_token(user_id, tok.id);
    }

    db_.erase_query<Request>(odb::query<Request>::user_id == user_id);
    db_.erase_query<RequestTotal>(odb::query<RequestTotal>::id.user_id == user_id);
    SessionStore::instance().delete_all_for_user(user_id);
    db_.erase(*p);
    t.commit();
    return true;
}

double UserStore::get_user_balance_usd(long long user_id)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    if (user_id <= 0) {
        return 0;
    }
    ScopedTransaction t(db_);
    auto p = db_.find<User>(user_id);
    t.commit();
    return p ? p->balance_usd : 0;
}

bool UserStore::has_positive_user_balance(long long user_id)
{
    return get_user_balance_usd(user_id) > 0;
}

bool UserStore::debit_user_balance_usd(long long user_id, double delta_usd, double *remaining_usd)
{
    if (delta_usd <= 0) {
        if (remaining_usd != nullptr) {
            *remaining_usd = get_user_balance_usd(user_id);
        }
        return true;
    }

    ScopedTransaction t(db_);
    const auto raw =
        sql_query_one(db_, "SELECT balance_usd FROM users WHERE id=" + std::to_string(user_id) + " FOR UPDATE");
    const double balance = raw.has_value() ? std::stod(*raw) : 0;
    if (balance < delta_usd) {
        t.commit();
        if (remaining_usd != nullptr) {
            *remaining_usd = balance;
        }
        return false;
    }
    const double next = balance - delta_usd;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", next);
    sql_exec(db_, "UPDATE users SET balance_usd=" + std::string(buf) + " WHERE id=" + std::to_string(user_id));
    t.commit();
    if (remaining_usd != nullptr) {
        *remaining_usd = next;
    }
    return true;
}

} // namespace revlm
