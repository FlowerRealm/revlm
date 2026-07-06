#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "store/mysql.hpp"

namespace revlm
{

constexpr std::string_view setting_site_base_url = "site_base_url";
constexpr std::string_view setting_default_channel_group_id = "default_channel_group_id";
constexpr std::string_view setting_billing_paygo_price_multiplier = "billing_paygo_price_multiplier";

constexpr int price_multiplier_scale = 6;
constexpr std::string_view default_billing_paygo_price_multiplier = "1.000000";

struct AdminSettingsSnapshot {
    std::string mode = "business";

    std::string site_base_url;
    bool site_base_url_override = false;
    std::string site_base_url_effective;
    bool site_base_url_invalid = false;

    std::string billing_paygo_price_multiplier;
    bool billing_paygo_price_multiplier_override = false;
};

struct AdminSettingsUpdate {
    std::string site_base_url;
    std::string billing_paygo_price_multiplier;
};

struct RuntimeConfigVersion {
    unsigned long long version = 0;
    std::string updated_at;
};

std::string format_decimal_plain(std::string_view normalized, int scale);
std::string format_cny_fixed(std::string_view normalized);
std::string derive_base_url_from_request(std::string_view raw_request);

class AppSettingsStore {
public:
    explicit AppSettingsStore(MysqlConnection &conn);

    std::optional<std::string> get_string(std::string_view key);
    void upsert_string(std::string_view key, std::string_view value);
    void delete_key(std::string_view key);

    AdminSettingsSnapshot get_admin_settings(std::string_view raw_request);
    void update_admin_settings(const AdminSettingsUpdate &update);
    RuntimeConfigVersion runtime_config_version();

private:
    void bump_runtime_config_version();
    MysqlConnection &conn_;
};

} // namespace revlm
