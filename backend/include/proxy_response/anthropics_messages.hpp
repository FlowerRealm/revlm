#pragma once

#include "proxy_response/gateway.hpp"
#include <boost/json.hpp>
#include <boost/json/object.hpp>

namespace revlm
{

class AnthropicsMessages : public Gateway {
public:
    AnthropicsMessages(const Model &model, double tier_multiplier, double channel_multiplier)
        : Gateway(model, tier_multiplier, channel_multiplier)
    {
    }
    void finalize(boost::json::object &json) override;
};

} // namespace revlm
