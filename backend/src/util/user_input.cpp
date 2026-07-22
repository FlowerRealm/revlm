#include "util/user_input.hpp"

#include "auth/crypto.hpp"
#include "util/strings.hpp"

#include <crypt.h>

#include <charconv>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace revlm
{
namespace
{

constexpr int cny_scale = 2;

} // namespace

void require_password_length(std::string_view password)
{
    if (password.size() < 8)
        throw std::invalid_argument("密码长度至少 8 位");
}

namespace
{

std::string bcrypt_salt()
{
    static constexpr char alphabet[] = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    const std::string raw = random_bytes(16);
    std::string salt = "$2b$12$";
    int bits = 0;
    unsigned int acc = 0;
    for (unsigned char ch : raw) {
        acc |= static_cast<unsigned int>(ch) << bits;
        bits += 8;
        while (bits >= 6 && salt.size() < 29) {
            salt.push_back(alphabet[acc & 0x3f]);
            acc >>= 6;
            bits -= 6;
        }
    }
    if (salt.size() < 29)
        salt.push_back(alphabet[acc & 0x3f]);
    salt.resize(29, '.');
    return salt;
}

} // namespace

std::string hash_password(std::string_view password)
{
    require_password_length(password);
    const std::string salt = bcrypt_salt();
    crypt_data data{};
    data.initialized = 0;
    char *hash = ::crypt_r(std::string{ password }.c_str(), salt.c_str(), &data);
    if (hash == nullptr || std::strncmp(hash, "$2", 2) != 0)
        throw std::runtime_error("密码哈希失败");
    return std::string{ hash };
}

bool check_password(std::string_view hash, std::string_view password)
{
    if (!hash.starts_with("$2"))
        return false;
    crypt_data data{};
    data.initialized = 0;
    char *got = ::crypt_r(std::string{ password }.c_str(), std::string{ hash }.c_str(), &data);
    if (got == nullptr)
        return false;
    const std::string got_text{ got };
    return constant_time_equal(got_text, hash);
}

std::string normalize_username(std::string_view raw)
{
    std::string username = trim_ascii(raw);
    if (username.empty())
        throw std::invalid_argument("账号名不能为空");
    if (username.size() > 64)
        throw std::invalid_argument("账号名长度不能超过 64 位");
    for (char ch : username)
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')))
            throw std::invalid_argument("账号名仅支持字母/数字（区分大小写），不允许空格或特殊字符");
    return username;
}

std::string normalize_email(std::string_view raw)
{
    std::string email = lowercase_ascii(trim_ascii(raw));
    if (email.empty())
        throw std::invalid_argument("邮箱不能为空");
    const size_t at = email.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= email.size() || email.find('@', at + 1) != std::string::npos ||
        email.find('.', at + 1) == std::string::npos)
        throw std::invalid_argument("邮箱不合法");
    return email;
}

std::string normalize_user_role(std::string_view raw, std::string_view fallback)
{
    std::string role = trim_ascii(raw);
    if (role.empty())
        role = trim_ascii(fallback);
    if (role.empty())
        role = "user";
    if (role != "user" && role != "root")
        throw std::invalid_argument("role 不合法");
    return role;
}

int normalize_user_status(int raw)
{
    if (raw != 0 && raw != 1)
        throw std::invalid_argument("status 不合法");
    return raw;
}

std::string normalize_usd_amount(std::string_view raw)
{
    std::string text = trim_ascii(raw);
    if (text.empty())
        throw std::invalid_argument("金额不能为空");
    bool seen_dot = false;
    int fraction_digits = 0;
    int digit_count = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '+' && i == 0)
            continue;
        if (ch == '.') {
            if (seen_dot)
                throw std::invalid_argument("金额格式不合法");
            seen_dot = true;
            continue;
        }
        if (ch < '0' || ch > '9')
            throw std::invalid_argument("金额格式不合法");
        ++digit_count;
        if (seen_dot)
            if (++fraction_digits > 6)
                throw std::invalid_argument("金额最多支持 6 位小数");
    }
    if (digit_count == 0)
        throw std::invalid_argument("金额格式不合法");
    long double value = 0.0L;
    try {
        value = std::stold(text);
    } catch (const std::exception &) {
        throw std::invalid_argument("金额格式不合法");
    }
    if (!(value > 0.0L))
        throw std::invalid_argument("金额必须大于 0");
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    return format_usd_plain_fixed6(out.str());
}

std::string normalize_decimal(std::string_view raw, int scale, int max_integer_digits, bool allow_zero,
                              std::string_view field_name)
{
    std::string value = trim_ascii(raw);
    if (!value.empty() && value[0] == '+')
        value.erase(value.begin());
    if (value.empty())
        throw std::invalid_argument(std::string{ field_name } + " is required");
    if (value[0] == '-')
        throw std::invalid_argument(std::string{ field_name } + " must be non-negative");
    const size_t dot = value.find('.');
    if (dot != std::string::npos && value.find('.', dot + 1) != std::string::npos)
        throw std::invalid_argument(std::string{ field_name } + " is invalid");

    std::string int_part = dot == std::string::npos ? value : value.substr(0, dot);
    std::string frac_part = dot == std::string::npos ? "" : value.substr(dot + 1);
    if (int_part.empty())
        int_part = "0";
    bool has_digit = false;
    for (char ch : int_part) {
        if (ch < '0' || ch > '9')
            throw std::invalid_argument(std::string{ field_name } + " is invalid");
        has_digit = true;
    }
    for (char ch : frac_part) {
        if (ch < '0' || ch > '9')
            throw std::invalid_argument(std::string{ field_name } + " is invalid");
        has_digit = true;
    }
    if (!has_digit) {
        throw std::invalid_argument(std::string{ field_name } + " is invalid");
    }

    size_t first_non_zero = int_part.find_first_not_of('0');
    if (first_non_zero == std::string::npos)
        int_part = "0";
    else
        int_part.erase(0, first_non_zero);
    if (static_cast<int>(int_part.size()) > max_integer_digits)
        throw std::invalid_argument(std::string{ field_name } + " is too large");
    if (static_cast<int>(frac_part.size()) > scale)
        throw std::invalid_argument(std::string{ field_name } + " has too many decimal places");
    while (static_cast<int>(frac_part.size()) < scale)
        frac_part.push_back('0');

    const std::string normalized = int_part + "." + frac_part;
    if (!allow_zero && normalized == std::string{ "0." } + std::string(static_cast<size_t>(scale), '0'))
        throw std::invalid_argument(std::string{ field_name } + " must be greater than zero");
    return normalized;
}

std::string normalize_money_non_negative(std::string_view raw, int scale, std::string_view field_name)
{
    return normalize_decimal(raw, scale, 18, true, field_name);
}

std::string normalize_cny_amount(std::string_view raw)
{
    std::string value = trim_ascii(raw);
    if (!value.empty() && value[0] == '$')
        value.erase(value.begin());
    if (value.rfind("\xC2\xA5", 0) == 0)
        value.erase(0, 2);
    if (value.rfind("\xEF\xBF\xA5", 0) == 0)
        value.erase(0, 3);
    return normalize_money_non_negative(value, cny_scale, "cny_amount");
}

std::string normalize_http_base_url(std::string value, std::string_view key)
{
    value = trim_ascii(std::move(value));
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    if (value.empty())
        return {};
    const bool http = value.rfind("http://", 0) == 0;
    const bool https = value.rfind("https://", 0) == 0;
    if (!http && !https)
        throw std::invalid_argument(std::string{ key } + " must start with http:// or https://");
    const size_t scheme_end = value.find("://");
    const size_t host_start = scheme_end == std::string::npos ? std::string::npos : scheme_end + 3;
    const size_t host_end = host_start == std::string::npos ? std::string::npos :
                                                              value.find_first_of("/?#", host_start);
    if (host_start == std::string::npos || host_start >= value.size() || host_end == host_start)
        throw std::invalid_argument(std::string{ key } + " host must not be empty");
    return value;
}

std::string normalize_channel_group_name(std::string_view raw)
{
    std::string name = trim_ascii(raw);
    if (name.empty())
        throw std::invalid_argument("渠道组名不能为空");
    if (name.size() > 64)
        throw std::invalid_argument("渠道组名过长");
    for (char ch : name)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
              ch == '-'))
            throw std::invalid_argument("渠道组名包含非法字符");
    return name;
}

std::vector<std::string> normalize_token_channel_groups(const std::vector<std::string> &names)
{
    std::unordered_set<std::string> seen;
    std::vector<std::string> out;
    out.reserve(names.size());
    for (const std::string &raw : names) {
        const std::string trimmed = trim_ascii(raw);
        if (trimmed.empty())
            continue;
        const std::string name = normalize_channel_group_name(trimmed);
        if (seen.insert(name).second)
            out.push_back(name);
    }
    if (out.size() > 20)
        out.resize(20);
    return out;
}

void require_positive(long long value, std::string_view name)
{
    if (value <= 0)
        throw std::invalid_argument(std::string{ name } + " must be positive");
}

std::optional<long long> require_positive_i64(std::string_view raw)
{
    std::string value = trim_ascii(raw);
    if (value.empty())
        return std::nullopt;
    long long out = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    if (ec != std::errc{} || ptr != value.data() + value.size() || out <= 0)
        throw std::invalid_argument("invalid positive integer");
    return out;
}

std::optional<int> require_positive_i32(std::string_view raw)
{
    auto value = require_positive_i64(raw);
    if (!value.has_value())
        return std::nullopt;
    if (*value > std::numeric_limits<int>::max())
        throw std::invalid_argument("invalid integer range");
    return static_cast<int>(*value);
}

std::optional<long long> parse_positive_i64_or(std::string_view raw)
{
    const std::string trimmed = trim_ascii(raw);
    if (trimmed.empty())
        return std::nullopt;
    long long value = 0;
    for (char ch : trimmed) {
        if (ch < '0' || ch > '9')
            return std::nullopt;
        const int digit = ch - '0';
        if (value > (std::numeric_limits<long long>::max() - digit) / 10)
            return std::nullopt;
        value = value * 10 + digit;
    }
    if (value <= 0)
        return std::nullopt;
    return value;
}

std::optional<long long> parse_u64_digits_or(std::string_view raw)
{
    const std::string value = trim_ascii(raw);
    if (value.empty())
        return std::nullopt;
    long long id = 0;
    for (char ch : value) {
        if (ch < '0' || ch > '9')
            return std::nullopt;
        const int digit = ch - '0';
        if (id > (std::numeric_limits<long long>::max() - digit) / 10)
            return std::nullopt;
        id = id * 10 + digit;
    }
    return id;
}

bool parse_i64(std::string_view raw, long long &out)
{
    const std::string value = trim_ascii(raw);
    if (value.empty())
        return false;
    size_t pos = 0;
    try {
        out = std::stoll(value, &pos, 10);
    } catch (const std::exception &) {
        return false;
    }
    return pos == value.size();
}

bool parse_i32(std::string_view raw, int &out)
{
    long long value = 0;
    if (!parse_i64(raw, value) || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        return false;
    out = static_cast<int>(value);
    return true;
}

std::optional<long long> parse_long_long(std::string_view raw)
{
    const std::string trimmed = trim_ascii(raw);
    if (trimmed.empty())
        return std::nullopt;
    size_t pos = 0;
    long long out = 0;
    try {
        out = std::stoll(trimmed, &pos, 10);
    } catch (const std::exception &) {
        return std::nullopt;
    }
    if (pos != trimmed.size())
        return std::nullopt;
    return out;
}

std::optional<int> parse_int_value(std::string_view raw)
{
    const auto got = parse_long_long(raw);
    if (!got.has_value() || *got < std::numeric_limits<int>::min() || *got > std::numeric_limits<int>::max())
        return std::nullopt;
    return static_cast<int>(*got);
}

bool parse_bool_flag(std::string_view raw, bool &out)
{
    const std::string_view value = trim_ascii(raw);
    if (value == "1" || value == "true" || value == "yes") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no") {
        out = false;
        return true;
    }
    return false;
}

std::optional<bool> parse_bool_value(std::string_view raw)
{
    const std::string_view value = trim_ascii(raw);
    if (value == "1" || value == "true" || value == "yes" || value == "on")
        return true;
    if (value == "0" || value == "false" || value == "no" || value == "off")
        return false;
    return std::nullopt;
}

bool is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int days_in_month(int year, int month)
{
    static constexpr int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12)
        return 0;
    if (month == 2 && is_leap_year(year))
        return 29;
    return days[month - 1];
}

bool parse_date_yyyy_mm_dd(std::string_view raw, int &year, int &month, int &day)
{
    if (raw.size() != 10 || raw[4] != '-' || raw[7] != '-')
        return false;
    if (!parse_i32(raw.substr(0, 4), year) || !parse_i32(raw.substr(5, 2), month) || !parse_i32(raw.substr(8, 2), day))
        return false;
    if (year < 1970 || month < 1 || month > 12)
        return false;
    const int dim = days_in_month(year, month);
    return day >= 1 && day <= dim;
}

} // namespace revlm
