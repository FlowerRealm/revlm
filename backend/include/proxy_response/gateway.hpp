#pragma once

#include "request/request.hpp"
#include <boost/json.hpp>
#include <boost/json/object.hpp>

namespace revlm
{

class Gateway {
public:
    Gateway(const Model &model, double tier_multiplier, double channel_multiplier)
        : request(model, 0, 0, 0, 0, 0, tier_multiplier, channel_multiplier)
    {
    }
    virtual ~Gateway() = default;
    virtual void finalize(boost::json::object &json) = 0;

protected:
    Request request;
};

} // namespace revlm
