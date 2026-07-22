#pragma once

#include <httplib.h>

#include <functional>
#include <string_view>

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

protected:
    bool channel_ok(const Channel &channel) const override;
    GatewayStreamKind kind() const override;
    std::string_view no_available_channel_message() const override;
    std::string_view upstream_path() const override;
};

json run_messages(ProxyRequest &pr);
void run_messages_stream(::httplib::Response &res, ProxyRequest pr,
                         const std::function<void(ProxyRequest &)> &on_usage);

} // namespace revlm
