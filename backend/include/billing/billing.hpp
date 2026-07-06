#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "store/mysql.hpp"

namespace revlm
{

constexpr int usd_scale = 6;

class BillingStore {
public:
    explicit BillingStore(MysqlConnection &conn);

    std::string get_user_balance_usd(long long user_id);
    bool has_positive_user_balance(long long user_id);
    bool debit_user_balance_usd(long long user_id, double delta_usd, std::string *remaining_usd = nullptr);

private:
    MysqlConnection &conn_;
};

bool decimal_greater_than_zero(std::string_view normalized);
std::string billing_format_usd_from_micros(long long micros);

} // namespace revlm
