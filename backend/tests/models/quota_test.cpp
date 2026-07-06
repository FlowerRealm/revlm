#include "models/models.hpp"
#include "usage/usage_charge.hpp"

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

    revlm::UsageCommitPayload payload;
    payload.request_id = "req-quota-unit";
    payload.user_id = 7;
    payload.token_id = 8;
    revlm::fill_payload_from_request(payload, req);
    if (expect(payload.input_tokens == 1'000'000, "payload should copy input tokens") != 0 ||
        expect(payload.output_tokens == 500'000, "payload should copy output tokens") != 0 ||
        expect(payload.cache_read_input_tokens == 250'000, "payload should copy cache read tokens") != 0 ||
        expect(payload.cache_creation_input_tokens == 50'000, "payload should copy cache creation 5m tokens") != 0 ||
        expect(payload.cache_creation_1h_input_tokens == 100'000, "payload should copy cache creation 1h tokens") !=
            0 ||
        expect(payload.committed_usd == "3.881250", "payload committed_usd should match solve_price") != 0 ||
        expect(payload.price_multiplier_group == "1.500000", "payload should record channel multiplier") != 0 ||
        expect(payload.price_multiplier == "1.800000", "payload should record combined tier and channel multipliers") !=
            0) {
        return 1;
    }

    return 0;
}
