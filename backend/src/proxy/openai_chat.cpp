#include "proxy/openai_chat.hpp"

#include <functional>
#include <httplib.h>

namespace revlm
{

void OpenaiChatCompletion::finalize(json &json_obj)
{
    // Official CompletionUsage: prompt_tokens includes cached_tokens (subset);
    // prompt_tokens_details may omit fields — treat nested as optional.
    const json usage = json_obj["usage"];
    const long long prompt_tokens = usage["prompt_tokens"].as_int64().value();
    const long long completion_tokens = usage["completion_tokens"].as_int64().value();
    const json details = usage["prompt_tokens_details"];
    const long long cached_tokens = details.is_object() ? details["cached_tokens"].as_int64().value_or(0) : 0;
    const long long cache_write_tokens = details.is_object() ? details["cache_write_tokens"].as_int64().value_or(0) : 0;
    request.usage.input_tokens = static_cast<int>(prompt_tokens - cached_tokens);
    request.usage.output_tokens = static_cast<int>(completion_tokens);
    request.usage.cache_read_tokens = static_cast<int>(cached_tokens);
    request.usage.cache_creation_1h_tokens = 0; // OpenAI has no 1h/5m split
    request.usage.cache_creation_5m_tokens = static_cast<int>(cache_write_tokens);
    if (const auto tier = json_obj["service_tier"].as_string(); tier.has_value()) {
        request.upstream.service_tier = *tier;
    }
    if (const auto model = json_obj["model"].as_string(); model.has_value() && !model->empty()) {
        request.upstream.model_name = *model;
    }
}

bool OpenaiChatCompletion::channel_ok(const Channel &channel) const
{
    return channel.status && channel.type == "openai_compatible" && !channel.api_key.empty();
}

GatewayStreamKind OpenaiChatCompletion::kind() const
{
    return GatewayStreamKind::openai_chat;
}

std::string_view OpenaiChatCompletion::upstream_path() const
{
    return "/v1/chat/completions";
}

json run_chat_completions(ProxyRequest &pr)
{
    return OpenaiChatCompletion(pr).run();
}

void run_chat_completions_stream(::httplib::Response &res, ProxyRequest pr,
                                 const std::function<void(ProxyRequest &)> &on_usage)
{
    OpenaiChatCompletion(pr).run_stream(res, on_usage);
}

} // namespace revlm
