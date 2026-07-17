#include "proxy_response/openai_responses.hpp"
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
    const long long input_tokens = json_int64_or(usage, "input_tokens");
    const long long output_tokens = json_int64_or(usage, "output_tokens");

    long long cached = json_int64_or(usage, "cache_read_input_tokens");
    if (const auto *details = usage.if_contains("input_tokens_details"); details != nullptr && details->is_object()) {
        const long long from_details = json_int64_or(details->as_object(), "cached_tokens");
        if (from_details > 0) {
            cached = from_details;
        }
    }

    const long long cache_creation_5m = json_int64_or(usage, "cache_creation_input_tokens");
    const long long cache_creation_1h = json_int64_or(usage, "cache_creation_1h_input_tokens");

    request.input_tokens = static_cast<int>(input_tokens);
    request.output_tokens = static_cast<int>(output_tokens);
    request.cache_read_tokens = static_cast<int>(cached);
    request.cache_creation_1h_tokens = static_cast<int>(cache_creation_1h);
    request.cache_creation_5m_tokens = static_cast<int>(cache_creation_5m);
}

} // namespace revlm
