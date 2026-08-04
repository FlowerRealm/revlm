#include "models/models.hpp"
#include "request/proxy_request.hpp"
#include "request/request.hpp"

#include <cmath>
#include <iostream>

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
    revlm::ProxyRequest req;
    req.protocol_cost_usd = 2.07;
    req.upstream.channel_group_multiplier = 1.5;
    req.token_details = R"({"usage":{"input_tokens":1000000,"output_tokens":500000,"cache_read_input_tokens":250000,)"
                        R"("cache_creation":{"ephemeral_5m_input_tokens":50000,"ephemeral_1h_input_tokens":100000}}})";
    revlm::Request stored;
    stored.token_details = req.token_details;
    const revlm::UsageTokens usage = revlm::usage_tokens(stored);
    const double price = req.protocol_cost_usd * req.upstream.channel_group_multiplier;
    if (expect(std::abs(price - 3.105) < 1e-9, "usd should apply protocol cost and channel group multiplier") != 0) {
        return 1;
    }

    if (expect(usage.input_tokens == 1'000'000, "request should keep input tokens") != 0 ||
        expect(usage.output_tokens == 500'000, "request should keep output tokens") != 0 ||
        expect(usage.cache_read_tokens == 250'000, "request should keep cache read tokens") != 0 ||
        expect(usage.cache_creation_5m_tokens == 50'000, "request should keep cache creation 5m tokens") != 0 ||
        expect(usage.cache_creation_1h_tokens == 100'000, "request should keep cache creation 1h tokens") != 0 ||
        expect(req.upstream.channel_group_multiplier == 1.5, "request should keep channel group multiplier") != 0 ||
        expect(std::abs(req.protocol_cost_usd * req.upstream.channel_group_multiplier - 3.105) < 1e-9,
               "request usd should stay stable") != 0) {
        return 1;
    }

    return 0;
}
