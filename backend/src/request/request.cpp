#include "request/request.hpp"

namespace revlm
{

Request::Request() = default;

double Request::solve_price() const
{
    return (model.input_price * input_tokens / 1000000 + model.output_price * output_tokens / 1000000 +
            model.cache_read_price * cache_read_tokens / 1000000 +
            model.cache_creation_1h_price * cache_creation_1h_tokens / 1000000 +
            model.cache_creation_5m_price * cache_creation_5m_tokens / 1000000) *
           tier_multiplier * channel_multiplier;
}

} // namespace revlm