#pragma once

#include <httplib.h>

#include <functional>
#include <string_view>

#include <boost/json/object.hpp>

#include "models/models.hpp"
#include "proxy/gateway.hpp"
#include "request/request.hpp"
#include "server/http_server.hpp"

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

struct ResponsesProxyExecuteOptions {
    int client_fd = -1;
    ClientWriter write_client;
    ::httplib::Response *stream_response = nullptr;
    std::function<void(Request &)> on_usage; // Path B only; Path C must be empty
};

struct ResponsesProxyResult {
    HttpResponse response;
    bool handled_stream = false;
    int stream_status = 0;
};

ResponsesProxyResult handle_responses_proxy_request(const ::httplib::Request &req, std::string_view method,
                                                    std::string_view path, std::string_view request_id,
                                                    long long channel_group_id, Request &usage,
                                                    const ResponsesProxyExecuteOptions &options = {});

} // namespace revlm
