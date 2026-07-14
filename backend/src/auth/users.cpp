#include "auth/users.hpp"

#include "auth/session.hpp"
#include "billing/billing.hpp"
#include "store/database.hpp"
#include "revlm_entities-odb.hxx"
#include "util/strings.hpp"
#include "util/user_input.hpp"

#include <odb/database.hxx>
#include <odb/query.hxx>
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

    *p = User(user.id, user.email, user.username, user.password_hash, user.role, user.status);
    db_.update(*p);

    if (!user.balance_usd.empty()) {
        const std::string amount = normalize_usd_amount(user.balance_usd);
        std::string amount_digits;
        for (char ch : amount) {
            if (ch != '.') {
                amount_digits.push_back(ch);
            }
        }
        const long long delta_micros = std::stoll(amount_digits.empty() ? "0" : amount_digits);

        auto bal = db_.find<UserBalanceRow>(user.id);
        if (!bal) {
            db_.persist(UserBalanceRow{ user.id, amount });
        } else {
            const std::string cur = format_usd_plain_fixed6(bal->usd);
            std::string cur_digits;
            for (char ch : cur) {
                if (ch != '.' && ch != '-') {
                    cur_digits.push_back(ch);
                }
            }
            const long long cur_micros = std::stoll(cur_digits.empty() ? "0" : cur_digits);
            bal->usd = billing_format_usd_from_micros(cur_micros + delta_micros);
            db_.update(*bal);
        }
    }

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
    db_.erase_query<UserBalanceRow>(odb::query<UserBalanceRow>::user_id == user_id);
    SessionStore(db_).delete_all_session_bindings(user_id);
    db_.erase(*p);
    t.commit();
    return true;
}

} // namespace revlm
