#include "config/app_settings.hpp"

#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "util/strings.hpp"
#include "util/user_input.hpp"

namespace revlm
{
namespace
{

constexpr int cny_scale = 2;
constexpr std::string_view setting_runtime_config_version = "_runtime_config_version";

std::string trim_fixed_decimal_zeros(std::string value)
{
    const size_t dot = value.find('.');
    if (dot == std::string::npos) {
        return value;
    }
    while (!value.empty() && value.back() == '0') {
        value.pop_back();
    }
    if (!value.empty() && value.back() == '.') {
        value.pop_back();
    }
    if (value.empty() || value == "-0") {
        return "0";
    }
    return value;
}

} // namespace

std::string format_decimal_plain(std::string_view normalized, int scale)
{
    std::string value = trim_ascii(normalized);
    const size_t dot = value.find('.');
    if (dot == std::string::npos) {
        return value.empty() ? "0" : value;
    }
    std::string int_part = value.substr(0, dot);
    std::string frac_part = value.substr(dot + 1);
    if (static_cast<int>(frac_part.size()) > scale) {
        frac_part.resize(static_cast<size_t>(scale));
    } else {
        while (static_cast<int>(frac_part.size()) < scale) {
            frac_part.push_back('0');
        }
    }
    return trim_fixed_decimal_zeros(int_part + "." + frac_part);
}

std::string format_cny_fixed(std::string_view normalized)
{
    std::string value = trim_ascii(normalized);
    const size_t dot = value.find('.');
    std::string int_part = dot == std::string::npos ? value : value.substr(0, dot);
    std::string frac_part = dot == std::string::npos ? "" : value.substr(dot + 1);
    if (int_part.empty()) {
        int_part = "0";
    }
    if (frac_part.size() > static_cast<size_t>(cny_scale)) {
        frac_part.resize(cny_scale);
    }
    while (frac_part.size() < static_cast<size_t>(cny_scale)) {
        frac_part.push_back('0');
    }
    return int_part + "." + frac_part;
}

std::string derive_base_url_from_request(std::string_view raw_request)
{
    auto request_header = [raw_request](std::string_view name) -> std::string_view {
        size_t line_start = 0;
        while (line_start < raw_request.size()) {
            const size_t line_end = raw_request.find("\r\n", line_start);
            if (line_end == std::string_view::npos || line_end == line_start) {
                break;
            }
            std::string_view line = raw_request.substr(line_start, line_end - line_start);
            line_start = line_end + 2;
            const size_t colon = line.find(':');
            if (colon == std::string_view::npos) {
                continue;
            }
            std::string_view key = line.substr(0, colon);
            if (key.size() != name.size()) {
                continue;
            }
            bool equal = true;
            for (size_t i = 0; i < key.size(); ++i) {
                char a = key[i];
                char b = name[i];
                if (a >= 'A' && a <= 'Z') {
                    a = static_cast<char>(a - 'A' + 'a');
                }
                if (b >= 'A' && b <= 'Z') {
                    b = static_cast<char>(b - 'A' + 'a');
                }
                if (a != b) {
                    equal = false;
                    break;
                }
            }
            if (!equal) {
                continue;
            }
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.remove_suffix(1);
            }
            return value;
        }
        return {};
    };

    const std::string forwarded_proto = trim_ascii(request_header("X-Forwarded-Proto"));
    const std::string forwarded_host = trim_ascii(request_header("X-Forwarded-Host"));
    const std::string host = trim_ascii(request_header("Host"));
    std::string scheme = lowercase_ascii(forwarded_proto.empty() ? "http" : forwarded_proto);
    if (scheme != "https") {
        scheme = "http";
    }
    const std::string authority = forwarded_host.empty() ? host : forwarded_host;
    if (authority.empty()) {
        return scheme + "://127.0.0.1";
    }
    return scheme + "://" + authority;
}

AppSettingsStore::AppSettingsStore(MysqlConnection &conn)
    : conn_(conn)
{
}

std::optional<std::string> AppSettingsStore::get_string(std::string_view key)
{
    const auto raw = conn_.query_one("SELECT value FROM app_settings WHERE `key`=" + conn_.quote(key));
    if (!raw.has_value()) {
        return std::nullopt;
    }
    return trim_ascii(*raw);
}

void AppSettingsStore::upsert_string(std::string_view key, std::string_view value)
{
    conn_.exec("INSERT INTO app_settings(`key`, value, created_at, updated_at) VALUES(" + conn_.quote(key) + ", " +
               conn_.quote(trim_ascii(value)) +
               ", CURRENT_TIMESTAMP, CURRENT_TIMESTAMP) "
               "ON DUPLICATE KEY UPDATE value=VALUES(value), updated_at=CURRENT_TIMESTAMP");
}

void AppSettingsStore::delete_key(std::string_view key)
{
    conn_.exec("DELETE FROM app_settings WHERE `key`=" + conn_.quote(key));
}

RuntimeConfigVersion AppSettingsStore::runtime_config_version()
{
    RuntimeConfigVersion out;
    const auto rows = conn_.query_rows("SELECT value,updated_at FROM app_settings WHERE `key`=" +
                                       conn_.quote(setting_runtime_config_version) + " LIMIT 1");
    if (rows.empty()) {
        return out;
    }
    const MysqlResultRow &row = rows.front();
    if (!row.empty() && row[0].has_value()) {
        try {
            out.version = std::stoull(trim_ascii(*row[0]));
        } catch (const std::exception &) {
            out.version = 0;
        }
    }
    if (row.size() > 1 && row[1].has_value()) {
        out.updated_at = *row[1];
    }
    return out;
}

void AppSettingsStore::bump_runtime_config_version()
{
    unsigned long long next = runtime_config_version().version + 1;
    upsert_string(setting_runtime_config_version, std::to_string(next));
}

AdminSettingsSnapshot AppSettingsStore::get_admin_settings(std::string_view raw_request)
{
    AdminSettingsSnapshot out;
    out.site_base_url_effective = derive_base_url_from_request(raw_request);
    out.billing_paygo_price_multiplier =
        format_decimal_plain(default_billing_paygo_price_multiplier, price_multiplier_scale);

    if (const auto site = get_string(setting_site_base_url); site.has_value()) {
        out.site_base_url_override = true;
        out.site_base_url = *site;
        try {
            out.site_base_url = normalize_http_base_url(*site, setting_site_base_url);
            if (!out.site_base_url.empty()) {
                out.site_base_url_effective = out.site_base_url;
            }
        } catch (const std::invalid_argument &) {
            out.site_base_url_invalid = true;
        }
    }

    if (const auto paygo = get_string(setting_billing_paygo_price_multiplier); paygo.has_value()) {
        try {
            const std::string normalized = normalize_price_multiplier_value(*paygo);
            out.billing_paygo_price_multiplier = format_decimal_plain(normalized, price_multiplier_scale);
            out.billing_paygo_price_multiplier_override = true;
        } catch (const std::invalid_argument &) {
            out.billing_paygo_price_multiplier =
                format_decimal_plain(default_billing_paygo_price_multiplier, price_multiplier_scale);
            out.billing_paygo_price_multiplier_override = false;
        }
    }

    return out;
}

void AppSettingsStore::update_admin_settings(const AdminSettingsUpdate &update)
{
    bool touched = false;

    const std::string site_base_url_raw = trim_ascii(update.site_base_url);
    if (site_base_url_raw.empty()) {
        if (get_string(setting_site_base_url).has_value()) {
            touched = true;
        }
        delete_key(setting_site_base_url);
    } else {
        const std::string normalized = normalize_http_base_url(site_base_url_raw, setting_site_base_url);
        touched = true;
        upsert_string(setting_site_base_url, normalized);
    }

    const std::string paygo_raw = trim_ascii(update.billing_paygo_price_multiplier);
    if (paygo_raw.empty()) {
        if (get_string(setting_billing_paygo_price_multiplier).has_value()) {
            touched = true;
        }
        delete_key(setting_billing_paygo_price_multiplier);
    } else {
        const std::string normalized = normalize_price_multiplier_value(paygo_raw);
        touched = true;
        upsert_string(setting_billing_paygo_price_multiplier, normalized);
    }

    if (touched) {
        bump_runtime_config_version();
    }
}

} // namespace revlm
