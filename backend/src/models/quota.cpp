#include "models/quota.hpp"
#include "errors/errors.hpp"

#include "auth/users.hpp"

namespace revlm
{

Quota::Quota()
    : db_(database())
{
}

void Quota::charge(Request request)
{
    const double price = request.solve_price();
    UserStore &users = UserStore::instance();
    if (!users.debit_user_balance_usd(request.user_id, price)) {
        throw QuotaInsufficientBalanceError(); // 余额不足
    }
}

} // namespace revlm
