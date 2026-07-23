#include "proxy/anthropics_messages.hpp"

#include "util/strings.hpp"

#include <functional>
#include <httplib.h>

namespace revlm
{

void AnthropicsMessages::finalize(json &json_obj)
{
    // Official: non-stream usage at root; SSE message_delta carries usage at event root
    // (also accept nested message.usage). cache_* optional when prompt caching unused.
    const json usage = json_obj["usage"].is_object() ? json_obj["usage"] : json_obj["message"]["usage"];
    request.usage.input_tokens = static_cast<int>(usage["input_tokens"].as_int64().value());
    request.usage.output_tokens = static_cast<int>(usage["output_tokens"].as_int64().value());
    request.usage.cache_read_tokens = static_cast<int>(usage["cache_read_input_tokens"].as_int64().value_or(0));
    const json cache_creation = usage["cache_creation"];
    if (cache_creation.is_object()) {
        request.usage.cache_creation_1h_tokens =
            static_cast<int>(cache_creation["ephemeral_1h_input_tokens"].as_int64().value_or(0));
        request.usage.cache_creation_5m_tokens =
            static_cast<int>(cache_creation["ephemeral_5m_input_tokens"].as_int64().value_or(0));
    } else {
        request.usage.cache_creation_1h_tokens = 0;
        request.usage.cache_creation_5m_tokens = 0;
    }
    const json model_src = json_obj["message"].is_object() ? json_obj["message"] : json_obj;
    if (const auto model = model_src["model"].as_string(); model.has_value() && !model->empty()) {
        request.upstream.model_name = *model;
    }
    if (const auto tier = usage["service_tier"].as_string(); tier.has_value()) {
        request.upstream.service_tier = normalize_usage_service_tier(std::string_view{ *tier });
    } else if (const auto tier = model_src["service_tier"].as_string(); tier.has_value()) {
        request.upstream.service_tier = normalize_usage_service_tier(std::string_view{ *tier });
    }
}

bool AnthropicsMessages::channel_ok(const Channel &channel) const
{
    return channel.status && channel.type == "anthropic" && !trim_ascii(channel.api_key).empty();
}

GatewayStreamKind AnthropicsMessages::kind() const
{
    return GatewayStreamKind::anthropics_messages;
}

std::string_view AnthropicsMessages::upstream_path() const
{
    return "/v1/messages";
}

json run_messages(ProxyRequest &pr)
{
    return AnthropicsMessages(pr).run();
}

void run_messages_stream(::httplib::Response &res, ProxyRequest pr, const std::function<void(ProxyRequest &)> &on_usage)
{
    AnthropicsMessages(pr).run_stream(res, on_usage);
}

} // namespace revlm
