#include "models/quota.hpp"
#include "errors/errors.hpp"

#include "billing/billing.hpp"

namespace revlm
{

Quota::Quota(odb::database &db)
    : db_(db)
{
}

void Quota::charge(Request request)
{
    const double price = request.solve_price();
    BillingStore billing(db_);
    if (!billing.debit_user_balance_usd(request.user_id, price)) {
        throw QuotaInsufficientBalanceError(); // 余额不足
    }
}

} // namespace revlm
