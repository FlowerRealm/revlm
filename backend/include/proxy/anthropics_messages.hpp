#pragma once

#include <httplib.h>

#include <functional>
#include <string_view>

#include "models/models.hpp"
#include "proxy/gateway.hpp"
#include "request/request.hpp"
#include "server/http_server.hpp"

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

HttpResponse run_messages_gateway(const ::httplib::Request &req, std::string_view request_id,
                                  long long channel_group_id, Request &usage);
void run_messages_stream(::httplib::Response &res, const ::httplib::Request &req, const GatewayParsedRequest &parsed,
                         std::string_view request_id, long long channel_group_id, std::string_view client_ip,
                         Request usage, const std::function<void(Request &)> &on_usage);

} // namespace revlm
