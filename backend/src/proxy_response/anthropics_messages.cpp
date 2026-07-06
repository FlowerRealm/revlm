#include "proxy_response/anthropics_messages.hpp"
#include "request/request.hpp"
#include <boost/json/object.hpp>

namespace revlm
{

void AnthropicsMessages::finalize(boost::json::object &json)
{
    boost::json::object usage = json["usage"].as_object();
    boost::json::object cache_creation = usage["cache_creation"].as_object();
    request = Request(request.model, usage["input_tokens"].as_int64(), usage["output_tokens"].as_int64(),
                      usage["cache_read_input_tokens"].as_int64(),
                      cache_creation["ephemeral_1h_input_tokens"].as_int64(),
                      cache_creation["ephemeral_5m_input_tokens"].as_int64(), request.tier_multiplier,
                      request.channel_multiplier);
}

} // namespace revlm
