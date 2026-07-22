#pragma once

#include <httplib.h>

#include <functional>

#include "proxy/gateway.hpp"
#include "request/proxy_request.hpp"
#include "util/json.hpp"

namespace revlm
{

class OpenaiResponses : public Gateway {
public:
    OpenaiResponses(ProxyRequest &pr)
        : Gateway(pr)
    {
    }
    void finalize(json &json) override;
};

struct ResponsesProxyExecuteOptions {
    int client_fd = -1;
    ClientWriter write_client;
    ::httplib::Response *stream_response = nullptr;
    std::function<void(ProxyRequest &)> on_usage; // Path B only; Path C must be empty
};

struct ResponsesProxyResult {
    bool handled_stream = false;
    int stream_status = 0;
};

ResponsesProxyResult handle_responses_proxy_request(ProxyRequest &pr, ::httplib::Response &res,
                                                    const ResponsesProxyExecuteOptions &options = {});

} // namespace revlm
