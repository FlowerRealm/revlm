#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace revlm
{

void require_password_length(std::string_view password);
std::string hash_password(std::string_view password);
bool check_password(std::string_view hash, std::string_view password);

std::string normalize_username(std::string_view raw);
std::string normalize_email(std::string_view raw);
std::string normalize_user_role(std::string_view raw, std::string_view fallback = "user");
int normalize_user_status(int raw);
std::string normalize_usd_amount(std::string_view raw);

std::string normalize_decimal(std::string_view raw, int scale, int max_integer_digits, bool allow_zero,
                              std::string_view field_name);
std::string normalize_money_non_negative(std::string_view raw, int scale, std::string_view field_name);
std::string normalize_cny_amount(std::string_view raw);
std::string normalize_http_base_url(std::string value, std::string_view key);

std::string normalize_channel_group_name(std::string_view raw);
std::vector<std::string> normalize_token_channel_groups(const std::vector<std::string> &names);

void require_positive(long long value, std::string_view name);
std::optional<long long> require_positive_i64(std::string_view raw);
std::optional<int> require_positive_i32(std::string_view raw);
std::optional<long long> parse_positive_i64_or(std::string_view raw);
std::optional<long long> parse_u64_digits_or(std::string_view raw);
bool parse_i64(std::string_view raw, long long &out);
bool parse_i32(std::string_view raw, int &out);
std::optional<long long> parse_long_long(std::string_view raw);
std::optional<int> parse_int_value(std::string_view raw);
bool parse_bool_flag(std::string_view raw, bool &out);
std::optional<bool> parse_bool_value(std::string_view raw);

bool is_leap_year(int year);
int days_in_month(int year, int month);
bool parse_date_yyyy_mm_dd(std::string_view raw, int &year, int &month, int &day);

} // namespace revlm
