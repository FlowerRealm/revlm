#pragma once

#include "proxy_response/gateway.hpp"
#include <boost/json.hpp>
#include <boost/json/object.hpp>

namespace revlm
{

class OpenaiResponses : public Gateway {
public:
    OpenaiResponses(Request &usage, const Model *model, double tier_multiplier, double channel_multiplier)
        : Gateway(usage, model, tier_multiplier, channel_multiplier)
    {
    }
    void finalize(boost::json::object &json) override;
};

} // namespace revlm
