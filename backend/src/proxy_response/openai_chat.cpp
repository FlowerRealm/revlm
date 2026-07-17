#include "proxy_response/openai_chat.hpp"
#include "request/request.hpp"
#include <boost/json/object.hpp>
#include <boost/json/string_view.hpp>

namespace revlm
{
namespace
{

long long json_int64_or(const boost::json::object &obj, boost::json::string_view key, long long fallback = 0)
{
    const auto *value = obj.if_contains(key);
    if (value == nullptr) {
        return fallback;
    }
    if (const auto *i = value->if_int64()) {
        return *i;
    }
    if (const auto *u = value->if_uint64()) {
        return static_cast<long long>(*u);
    }
    return fallback;
}

} // namespace

void OpenaiChatCompletion::finalize(boost::json::object &json)
{
    const auto *usage_value = json.if_contains("usage");
    if (usage_value == nullptr || !usage_value->is_object()) {
        return;
    }
    const boost::json::object &usage = usage_value->as_object();
    const long long prompt_tokens = json_int64_or(usage, "prompt_tokens");
    const long long completion_tokens = json_int64_or(usage, "completion_tokens");
    long long cached_tokens = 0;
    if (const auto *details = usage.if_contains("prompt_tokens_details"); details != nullptr && details->is_object()) {
        cached_tokens = json_int64_or(details->as_object(), "cached_tokens");
    }
    const long long input_tokens = prompt_tokens > cached_tokens ? prompt_tokens - cached_tokens : 0;
    request.input_tokens = static_cast<int>(input_tokens);
    request.output_tokens = static_cast<int>(completion_tokens);
    request.cache_read_tokens = static_cast<int>(cached_tokens);
    request.cache_creation_1h_tokens = 0;
    request.cache_creation_5m_tokens = 0;
}

} // namespace revlm
