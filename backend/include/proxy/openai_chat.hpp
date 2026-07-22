#pragma once

#include <httplib.h>

#include <functional>

#include "proxy/gateway.hpp"
#include "request/proxy_request.hpp"
#include "util/json.hpp"

namespace revlm
{

class OpenaiChatCompletion : public Gateway {
public:
    OpenaiChatCompletion(ProxyRequest &pr)
        : Gateway(pr)
    {
    }
    void finalize(json &json) override;
};

json run_chat_completions(ProxyRequest &pr);
void run_chat_completions_stream(::httplib::Response &res, ProxyRequest pr,
                                 const std::function<void(ProxyRequest &)> &on_usage);

} // namespace revlm
