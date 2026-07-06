#include "config/app_settings.hpp"
#include "util/user_input.hpp"

#include <iostream>
#include <stdexcept>

namespace
{

int expect(bool ok, const char *message)
{
    if (ok) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main()
{
    if (expect(revlm::normalize_http_base_url(" https://example.com/ ", "site_base_url") == "https://example.com",
               "site_base_url should trim and drop trailing slash") != 0 ||
        expect(revlm::normalize_cny_amount(" ¥12.3 ") == "12.30",
               "CNY amount should normalize currency prefix and scale") != 0 ||
        expect(revlm::normalize_price_multiplier_value("01.25") == "1.250000",
               "price multiplier should normalize leading zeros and scale") != 0) {
        return 1;
    }

    bool bad_base_url = false;
    try {
        (void)revlm::normalize_http_base_url("example.com", "site_base_url");
    } catch (const std::invalid_argument &) {
        bad_base_url = true;
    }
    if (expect(bad_base_url, "site_base_url must require http/https scheme") != 0) {
        return 1;
    }

    bool zero_multiplier = false;
    try {
        (void)revlm::normalize_price_multiplier_value("0");
    } catch (const std::invalid_argument &) {
        zero_multiplier = true;
    }
    if (expect(zero_multiplier, "price multiplier must stay positive") != 0) {
        return 1;
    }

    const std::string effective = revlm::derive_base_url_from_request("GET /api/admin/settings HTTP/1.1\r\n"
                                                                      "Host: internal.example.test\r\n"
                                                                      "X-Forwarded-Proto: https\r\n"
                                                                      "X-Forwarded-Host: app.example.com\r\n\r\n");
    if (expect(effective == "https://app.example.com", "effective base URL should prefer forwarded host/proto") != 0) {
        return 1;
    }

    if (expect(revlm::format_cny_fixed(revlm::normalize_cny_amount("10")) == "10.00",
               "cny value should keep fixed two-decimal format") != 0 ||
        expect(revlm::format_decimal_plain(revlm::normalize_price_multiplier_value("1.000000"),
                                           revlm::price_multiplier_scale) == "1",
               "default multiplier should render as plain decimal for admin payload") != 0) {
        return 1;
    }

    return 0;
}
