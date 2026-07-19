#pragma once

#include <httplib.h>

#include <functional>

#include "models/models.hpp"
#include "proxy/gateway.hpp"
#include "request/request.hpp"
#include "util/json.hpp"

namespace revlm
{

class AnthropicsMessages : public Gateway {
public:
    AnthropicsMessages(Request &usage, const Model *model, double tier_multiplier, double channel_multiplier)
        : Gateway(usage, model, tier_multiplier, channel_multiplier)
    {
    }
    void finalize(json &json) override;
};

json run_messages(json req, Request &usage);
void run_messages_stream(::httplib::Response &res, json req, Request usage,
                         const std::function<void(Request &)> &on_usage);

} // namespace revlm
