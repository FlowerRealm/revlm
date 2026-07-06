#include "usage/usage.hpp"
#include "usage/usage_queries.hpp"
#include "util/user_input.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

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
    revlm::UsageEvent event;
    event.forwarded_model = "gpt-4.1";
    event.upstream_response_model = "gpt-4.1-mini";
    if (expect(revlm::usage_model_mismatch(event), "mismatch should detect divergent models") != 0) {
        return 1;
    }

    if (expect(revlm::normalize_usage_service_tier(std::string_view{ " Fast " }) == "priority",
               "service tier should normalize legacy fast tier") != 0 ||
        expect(revlm::normalize_usage_service_tier_downgrade_reason(std::string_view{ " Capacity " }) == "capacity",
               "downgrade reason should trim and lowercase") != 0) {
        return 1;
    }

    const auto parsed_i64 = revlm::require_positive_i64("42");
    const auto parsed_int = revlm::require_positive_i32("7");
    if (expect(parsed_i64.has_value() && *parsed_i64 == 42, "positive i64 query should parse") != 0 ||
        expect(parsed_int.has_value() && *parsed_int == 7, "positive int query should parse") != 0 ||
        expect(!revlm::require_positive_i64("").has_value(), "empty query should stay optional") != 0) {
        return 1;
    }

    bool rejected_zero = false;
    try {
        (void)revlm::require_positive_i64("0");
    } catch (const std::invalid_argument &) {
        rejected_zero = true;
    }
    if (expect(rejected_zero, "non-positive query values should be rejected") != 0) {
        return 1;
    }

    revlm::UsageEventRow row;
    row.id = 9;
    row.time = "2026-06-25T12:00:00Z";
    row.request_id = "req-usage";
    row.token_id = 3;
    row.state = "committed";
    row.committed_usd = "1.250000";
    row.model = "gpt-4.1";
    row.requested_service_tier = "priority";
    row.service_tier = "standard";
    row.cache_read_input_tokens = 4;
    row.cache_creation_input_tokens = 5;
    row.output_tokens = 8;
    row.status_code = 200;
    row.latency_ms = 640;
    row.first_token_latency_ms = 120;

    const std::string user_json = revlm::usage_event_to_user_json(row);
    row.model_mismatch = true;
    const std::string admin_json = revlm::usage_event_to_admin_json(row);
    if (expect(user_json.find("\"service_tier_downgraded\":true") != std::string::npos,
               "user json should expose downgraded tier") != 0 ||
        expect(admin_json.find("\"model_mismatch\":true") != std::string::npos,
               "admin json should expose model mismatch") != 0 ||
        expect(admin_json.find("\"cached_tokens\":\"9\"") != std::string::npos,
               "admin json should aggregate cached tokens") != 0) {
        return 1;
    }

    return 0;
}
