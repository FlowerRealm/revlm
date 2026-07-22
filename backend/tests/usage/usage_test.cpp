#include "request/request.hpp"
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
    if (expect(revlm::normalize_usage_service_tier(std::string_view{ " Priority " }) == "Priority",
               "service tier should trim only, case preserved") != 0) {
        return 1;
    }
    if (expect(revlm::normalize_usage_service_tier(std::string_view{ "default" }) == "default",
               "default service tier should stay default") != 0) {
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

    return 0;
}
