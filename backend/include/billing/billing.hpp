#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace odb
{
class database;
}

namespace revlm
{

constexpr int usd_scale = 6;

#pragma db object table("user_balances")
struct UserBalanceRow {
#pragma db id
    long long user_id = 0;
    std::string usd;
};

class BillingStore {
public:
    explicit BillingStore(odb::database &db);

    std::string get_user_balance_usd(long long user_id);
    bool has_positive_user_balance(long long user_id);
    bool debit_user_balance_usd(long long user_id, double delta_usd, std::string *remaining_usd = nullptr);

private:
    odb::database &db_;
};

bool decimal_greater_than_zero(std::string_view normalized);
std::string billing_format_usd_from_micros(long long micros);

} // namespace revlm
