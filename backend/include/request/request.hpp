#pragma once

#include "models/models.hpp"
namespace revlm
{

class Request {
public:
    Request();
    Request(Model model, int input_tokens, int output_tokens, int cache_read_tokens, int cache_creation_1h_tokens,
            int cache_creation_5m_tokens, double tier_multiplier = 1.0, double channel_multiplier = 1.0)
        : model(model)
        , input_tokens(input_tokens)
        , output_tokens(output_tokens)
        , cache_read_tokens(cache_read_tokens)
        , cache_creation_1h_tokens(cache_creation_1h_tokens)
        , cache_creation_5m_tokens(cache_creation_5m_tokens)
        , tier_multiplier(tier_multiplier)
        , channel_multiplier(channel_multiplier)
    {
    }
    Model model;
    long long user_id;
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;
    int cache_creation_1h_tokens = 0;
    int cache_creation_5m_tokens = 0;
    double tier_multiplier = 1.0;
    double channel_multiplier = 1.0;
    double solve_price() const;

private:
};

} // namespace revlm