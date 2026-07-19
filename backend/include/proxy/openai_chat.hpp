#pragma once

#include <httplib.h>

#include <functional>

#include "models/models.hpp"
#include "proxy/gateway.hpp"
#include "request/request.hpp"
#include "util/json.hpp"

namespace revlm
{

class OpenaiChatCompletion : public Gateway {
public:
    OpenaiChatCompletion(Request &usage, const Model *model, double tier_multiplier, double channel_multiplier)
        : Gateway(usage, model, tier_multiplier, channel_multiplier)
    {
    }
    void finalize(json &json) override;
};

json run_chat_completions(json req, Request &usage);
void run_chat_completions_stream(::httplib::Response &res, json req, Request usage,
                                 const std::function<void(Request &)> &on_usage);

} // namespace revlm
