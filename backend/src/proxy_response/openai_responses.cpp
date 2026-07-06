#include "proxy_response/openai_responses.hpp"
#include "request/request.hpp"
#include <boost/json/object.hpp>

namespace revlm
{

void OpenaiResponses::finalize(boost::json::object &json)
{
    boost::json::object response = json["response"].as_object();
    boost::json::object usage = response["usage"].as_object();
    boost::json::object input_tokens_details = usage["input_tokens_details"].as_object();
    request = Request(request.model, usage["input_tokens"].as_int64(), usage["output_tokens"].as_int64(),
                      input_tokens_details["cached_tokens"].as_int64(), 0, 0, request.tier_multiplier,
                      request.channel_multiplier);
}

} // namespace revlm
