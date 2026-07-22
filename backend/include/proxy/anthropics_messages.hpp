#pragma once

#include <httplib.h>

#include <functional>

#include "proxy/gateway.hpp"
#include "request/proxy_request.hpp"
#include "util/json.hpp"

namespace revlm
{

class AnthropicsMessages : public Gateway {
public:
    AnthropicsMessages(ProxyRequest &pr)
        : Gateway(pr)
    {
    }
    void finalize(json &json) override;
};

json run_messages(ProxyRequest &pr);
void run_messages_stream(::httplib::Response &res, ProxyRequest pr,
                         const std::function<void(ProxyRequest &)> &on_usage);

} // namespace revlm
