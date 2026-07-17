#pragma once

#include "models/models.hpp"
#include "request/request.hpp"
#include <boost/json/object.hpp>

namespace revlm
{

class Gateway {
public:
    Gateway(Request &usage, const Model *model, double tier_multiplier, double channel_multiplier)
        : request(usage)
    {
        if (model != nullptr) {
            request.pricing_model = model;
            if (request.model_name.null() || request.model_name->empty()) {
                request.model_name = model->name;
            }
        }
        request.tier_multiplier = tier_multiplier;
        request.channel_multiplier = channel_multiplier;
    }
    virtual ~Gateway() = default;
    virtual void finalize(boost::json::object &json) = 0;

protected:
    Request &request;
};

} // namespace revlm
