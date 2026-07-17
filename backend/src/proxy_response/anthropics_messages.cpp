#include "proxy_response/anthropics_messages.hpp"
#include "request/request.hpp"
#include <boost/json/object.hpp>
#include <boost/json/string_view.hpp>
#include <optional>

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

std::optional<boost::json::object> extract_usage_object(const boost::json::value &value)
{
    if (value.is_object()) {
        const auto &obj = value.as_object();
        if (const auto *usage = obj.if_contains("usage"); usage != nullptr && usage->is_object()) {
            return usage->as_object();
        }
        for (const auto &field : obj) {
            if (auto nested = extract_usage_object(field.value())) {
                return nested;
            }
        }
        return std::nullopt;
    }
    if (value.is_array()) {
        for (const auto &child : value.as_array()) {
            if (auto nested = extract_usage_object(child)) {
                return nested;
            }
        }
    }
    return std::nullopt;
}

int merge_token(int current, long long incoming)
{
    if (incoming > 0) {
        return static_cast<int>(incoming);
    }
    return current;
}

} // namespace

void AnthropicsMessages::finalize(boost::json::object &json)
{
    auto usage_opt = extract_usage_object(json);
    if (!usage_opt.has_value()) {
        return;
    }
    const boost::json::object &usage = *usage_opt;
    long long ephemeral_1h = 0;
    long long ephemeral_5m = 0;
    if (const auto *cache_creation = usage.if_contains("cache_creation");
        cache_creation != nullptr && cache_creation->is_object()) {
        const boost::json::object &cache = cache_creation->as_object();
        ephemeral_1h = json_int64_or(cache, "ephemeral_1h_input_tokens");
        ephemeral_5m = json_int64_or(cache, "ephemeral_5m_input_tokens");
    }
    const int input_tokens = merge_token(request.input_tokens, json_int64_or(usage, "input_tokens"));
    const int output_tokens = merge_token(request.output_tokens, json_int64_or(usage, "output_tokens"));
    const int cache_read_tokens =
        merge_token(request.cache_read_tokens, json_int64_or(usage, "cache_read_input_tokens"));
    const int cache_creation_1h_tokens = merge_token(request.cache_creation_1h_tokens, ephemeral_1h);
    const int cache_creation_5m_tokens = merge_token(request.cache_creation_5m_tokens, ephemeral_5m);
    request.input_tokens = input_tokens;
    request.output_tokens = output_tokens;
    request.cache_read_tokens = cache_read_tokens;
    request.cache_creation_1h_tokens = cache_creation_1h_tokens;
    request.cache_creation_5m_tokens = cache_creation_5m_tokens;
}

} // namespace revlm
