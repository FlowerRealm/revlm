#pragma once

#include <httplib.h>

#include <functional>

#include "models/models.hpp"
#include "proxy/gateway.hpp"
#include "request/request.hpp"
#include "util/json.hpp"

namespace revlm
{

class OpenaiResponses : public Gateway {
public:
    OpenaiResponses(Request &usage, const Model *model, double tier_multiplier, double channel_multiplier)
        : Gateway(usage, model, tier_multiplier, channel_multiplier)
    {
    }
    void finalize(json &json) override;
};

struct ResponsesProxyExecuteOptions {
    int client_fd = -1;
    ClientWriter write_client;
    ::httplib::Response *stream_response = nullptr;
    std::function<void(Request &)> on_usage; // Path B only; Path C must be empty
};

struct ResponsesProxyResult {
    bool handled_stream = false;
    int stream_status = 0;
};

ResponsesProxyResult handle_responses_proxy_request(json req, ::httplib::Response &res, Request &usage,
                                                    const ResponsesProxyExecuteOptions &options = {});

} // namespace revlm
