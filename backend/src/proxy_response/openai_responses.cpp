#include "proxy_response/openai_responses.hpp"
#include "request/request.hpp"
#include <boost/json/object.hpp>

namespace revlm
{

void OpenaiResponses::finalize(boost::json::object &json)
{
    const boost::json::object *usage_parent = &json;
    if (const auto *response = json.if_contains("response"); response != nullptr && response->is_object()) {
        usage_parent = &response->as_object();
    }
    const auto *usage_value = usage_parent->if_contains("usage");
    if (usage_value == nullptr || !usage_value->is_object()) {
        return;
    }
    const boost::json::object &usage = usage_value->as_object();
    const auto *input = usage.if_contains("input_tokens");
    const auto *output = usage.if_contains("output_tokens");
    if (input == nullptr || !input->is_int64() || output == nullptr || !output->is_int64()) {
        return;
    }
    long long cached = 0;
    if (const auto *details = usage.if_contains("input_tokens_details"); details != nullptr && details->is_object()) {
        if (const auto *cached_tokens = details->as_object().if_contains("cached_tokens");
            cached_tokens != nullptr && cached_tokens->is_int64()) {
            cached = cached_tokens->as_int64();
        }
    }
    request = Request(request.model, input->as_int64(), output->as_int64(), cached, 0, 0, request.tier_multiplier,
                      request.channel_multiplier);
}

} // namespace revlm
