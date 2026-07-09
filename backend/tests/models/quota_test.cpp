#include "models/models.hpp"
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

revlm::Model test_model()
{
    return revlm::Model(1, "gpt-test", "openai", 1.0, 2.0, 0.5, 0.25, 0.125);
}

} // namespace

int main()
{
    const revlm::Model model = test_model();
    revlm::Request req(model, 1'000'000, 500'000, 250'000, 100'000, 50'000, 1.2, 1.5);
    const double price = req.solve_price();
    if (expect(std::abs(price - 3.88125) < 1e-9, "solve_price should apply token rates and multipliers") != 0) {
        return 1;
    }

    if (expect(req.input_tokens == 1'000'000, "request should keep input tokens") != 0 ||
        expect(req.output_tokens == 500'000, "request should keep output tokens") != 0 ||
        expect(req.cache_read_tokens == 250'000, "request should keep cache read tokens") != 0 ||
        expect(req.cache_creation_5m_tokens == 50'000, "request should keep cache creation 5m tokens") != 0 ||
        expect(req.cache_creation_1h_tokens == 100'000, "request should keep cache creation 1h tokens") != 0 ||
        expect(req.tier_multiplier == 1.2, "request should keep tier multiplier") != 0 ||
        expect(req.channel_multiplier == 1.5, "request should keep channel multiplier") != 0 ||
        expect(std::abs(req.solve_price() - 3.88125) < 1e-9, "request solve_price should stay stable") != 0) {
        return 1;
    }

    return 0;
}
