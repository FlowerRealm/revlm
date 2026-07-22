#pragma once

#include <httplib.h>

#include <string_view>

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

protected:
    bool channel_ok(const Channel &channel) const override;
    GatewayStreamKind kind() const override;
    std::string_view no_available_channel_message() const override;
    std::string_view upstream_path() const override;
    UpstreamRequest make_upstream(bool stream) const override;
    void fill_success_pricing(ProxyRequest &pr, const Channel &channel) override;
    bool should_bill_non_stream() const override;
    bool prepare(::httplib::Response &res) override;
};

ResponsesProxyResult handle_responses_proxy_request(ProxyRequest &pr, ::httplib::Response &res);
ResponsesProxyResult handle_responses_proxy_request(ProxyRequest &pr, ::httplib::Response &res,
                                                    const ResponsesProxyExecuteOptions &options);

} // namespace revlm
