#include "proxy_response/openai_chat.hpp"
#include "request/request.hpp"
#include <boost/json/object.hpp>

namespace revlm
{

void OpenaiChatCompletion::finalize(boost::json::object &json)
{
    boost::json::object usage = json["usage"].as_object();
    boost::json::object prompt_tokens_details = usage["prompt_tokens_details"].as_object();
    request = Request(request.model,
                      usage["prompt_tokens"].as_int64() - prompt_tokens_details["cached_tokens"].as_int64(),
                      usage["completion_tokens"].as_int64(), prompt_tokens_details["cached_tokens"].as_int64(), 0, 0,
                      request.tier_multiplier, request.channel_multiplier);
}

} // namespace revlm
