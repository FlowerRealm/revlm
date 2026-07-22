#include "models/models.hpp"
#include "request/proxy_request.hpp"

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

revlm::Model test_model()
{
    return revlm::Model(1, "gpt-test", "openai", 1.0, 2.0, 0.5, 0.25, 0.125);
}

} // namespace

int main()
{
    const revlm::Model model = test_model();
    revlm::ProxyRequest req;
    fill_pricing_from_model(req.upstream.pricing, model);
    req.usage.input_tokens = 1'000'000;
    req.usage.output_tokens = 500'000;
    req.usage.cache_read_tokens = 250'000;
    req.usage.cache_creation_1h_tokens = 100'000;
    req.usage.cache_creation_5m_tokens = 50'000;
    req.upstream.tier_multiplier = 1.2;
    req.upstream.channel_multiplier = 1.5;
    const double price = compute_usd(req);
    if (expect(std::abs(price - 3.88125) < 1e-9, "compute_usd should apply token rates and multipliers") != 0) {
        return 1;
    }

    if (expect(req.usage.input_tokens == 1'000'000, "request should keep input tokens") != 0 ||
        expect(req.usage.output_tokens == 500'000, "request should keep output tokens") != 0 ||
        expect(req.usage.cache_read_tokens == 250'000, "request should keep cache read tokens") != 0 ||
        expect(req.usage.cache_creation_5m_tokens == 50'000, "request should keep cache creation 5m tokens") != 0 ||
        expect(req.usage.cache_creation_1h_tokens == 100'000, "request should keep cache creation 1h tokens") != 0 ||
        expect(req.upstream.tier_multiplier == 1.2, "request should keep tier multiplier") != 0 ||
        expect(req.upstream.channel_multiplier == 1.5, "request should keep channel multiplier") != 0 ||
        expect(std::abs(compute_usd(req) - 3.88125) < 1e-9, "request compute_usd should stay stable") != 0) {
        return 1;
    }

    return 0;
}
