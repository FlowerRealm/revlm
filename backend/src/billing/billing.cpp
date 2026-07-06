#include "billing/billing.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

std::string normalize_stored_usd(std::string_view raw)
{
    const std::string value = trim_ascii(raw);
    if (value.empty()) {
        return "0.000000";
    }
    std::string int_part;
    std::string frac_part;
    bool seen_dot = false;
    for (char ch : value) {
        if (ch == '.') {
            if (seen_dot) {
                return "0.000000";
            }
            seen_dot = true;
            continue;
        }
        if (ch < '0' || ch > '9') {
            return "0.000000";
        }
        if (seen_dot) {
            frac_part.push_back(ch);
        } else {
            int_part.push_back(ch);
        }
    }
    if (int_part.empty()) {
        int_part = "0";
    }
    if (static_cast<int>(frac_part.size()) > usd_scale) {
        frac_part.resize(static_cast<size_t>(usd_scale));
    }
    while (static_cast<int>(frac_part.size()) < usd_scale) {
        frac_part.push_back('0');
    }
    return int_part + "." + frac_part;
}

__int128 parse_scaled_units(std::string_view normalized, int scale)
{
    const std::string value = trim_ascii(normalized);
    const size_t dot = value.find('.');
    std::string int_part = dot == std::string::npos ? value : value.substr(0, dot);
    std::string frac_part = dot == std::string::npos ? "" : value.substr(dot + 1);
    if (int_part.empty()) {
        int_part = "0";
    }
    while (static_cast<int>(frac_part.size()) < scale) {
        frac_part.push_back('0');
    }
    if (static_cast<int>(frac_part.size()) > scale) {
        frac_part.resize(static_cast<size_t>(scale));
    }
    __int128 out = 0;
    for (char ch : int_part) {
        if (ch < '0' || ch > '9') {
            throw std::invalid_argument("decimal is invalid");
        }
        out = out * 10 + (ch - '0');
    }
    for (char ch : frac_part) {
        if (ch < '0' || ch > '9') {
            throw std::invalid_argument("decimal is invalid");
        }
        out = out * 10 + (ch - '0');
    }
    return out;
}

std::string format_units(__int128 units, int scale)
{
    if (units < 0) {
        throw std::invalid_argument("units must be non-negative");
    }
    std::string digits;
    if (units == 0) {
        digits = "0";
    } else {
        while (units > 0) {
            const int digit = static_cast<int>(units % 10);
            digits.push_back(static_cast<char>('0' + digit));
            units /= 10;
        }
        std::reverse(digits.begin(), digits.end());
    }
    if (scale == 0) {
        return digits;
    }
    if (digits.size() <= static_cast<size_t>(scale)) {
        digits.insert(0, static_cast<size_t>(scale + 1 - digits.size()), '0');
    }
    digits.insert(digits.end() - scale, '.');
    return digits;
}

} // namespace

BillingStore::BillingStore(MysqlConnection &conn)
    : conn_(conn)
{
}

std::string BillingStore::get_user_balance_usd(long long user_id)
{
    assert(user_id > 0 && "internal: user_id must be positive");
    if (user_id <= 0) {
        return "0.000000";
    }
    const auto value = conn_.query_one("SELECT usd FROM user_balances WHERE user_id=" + std::to_string(user_id));
    return value.has_value() ? normalize_stored_usd(*value) : "0.000000";
}

bool BillingStore::has_positive_user_balance(long long user_id)
{
    return decimal_greater_than_zero(get_user_balance_usd(user_id));
}

bool BillingStore::debit_user_balance_usd(long long user_id, double delta_usd, std::string *remaining_usd)
{
    if (delta_usd <= 0) {
        if (remaining_usd != nullptr) {
            *remaining_usd = get_user_balance_usd(user_id);
        }
        return true;
    }
    char delta_buf[32];
    std::snprintf(delta_buf, sizeof(delta_buf), "%.6f", delta_usd);
    const __int128 delta_units = parse_scaled_units(delta_buf, usd_scale);
    DbTransaction tx(conn_);
    conn_.exec("INSERT IGNORE INTO user_balances(user_id, usd, created_at, updated_at) VALUES(" +
               std::to_string(user_id) + ", 0, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)");
    const std::string balance = normalize_stored_usd(
        conn_.query_one("SELECT usd FROM user_balances WHERE user_id=" + std::to_string(user_id) + " FOR UPDATE")
            .value_or("0.000000"));
    const __int128 balance_units = parse_scaled_units(balance, usd_scale);
    if (balance_units < delta_units) {
        tx.commit();
        if (remaining_usd != nullptr) {
            *remaining_usd = balance;
        }
        return false;
    }
    const std::string next = format_units(balance_units - delta_units, usd_scale);
    conn_.exec("UPDATE user_balances SET usd=" + conn_.quote(next) +
               ", updated_at=CURRENT_TIMESTAMP WHERE user_id=" + std::to_string(user_id));
    tx.commit();
    if (remaining_usd != nullptr) {
        *remaining_usd = next;
    }
    return true;
}

bool decimal_greater_than_zero(std::string_view normalized)
{
    try {
        return parse_scaled_units(normalize_stored_usd(normalized), usd_scale) > 0;
    } catch (...) {
        return false;
    }
}

std::string billing_format_usd_from_micros(long long micros)
{
    return format_units(static_cast<__int128>(micros), usd_scale);
}

} // namespace revlm
