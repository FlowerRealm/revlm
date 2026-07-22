#include "proxy/openai_responses.hpp"

#include "models/models.hpp"
#include "proxy/upstream.hpp"
#include "util/strings.hpp"

#include <httplib.h>
#include <string>
#include <string_view>
#include <vector>

namespace revlm
{

void OpenaiResponses::finalize(json &json_obj)
{
    // Official: non-stream usage at root; SSE nests under response.usage.
    // cached_tokens is a subset of input_tokens; cache_write_tokens is optional nested.
    const json &root = json_obj;
    const json response = root["response"];
    const json usage = (response.is_object() && response["usage"].is_object()) ? response["usage"] : root["usage"];
    const long long input_tokens = usage["input_tokens"].as_int64().value();
    const long long output_tokens = usage["output_tokens"].as_int64().value();
    const json details = usage["input_tokens_details"];
    const long long cached_tokens = details.is_object() ? details["cached_tokens"].as_int64().value_or(0) : 0;
    const long long cache_write_tokens = details.is_object() ? details["cache_write_tokens"].as_int64().value_or(0) : 0;
    request.usage.input_tokens = static_cast<int>(input_tokens - cached_tokens);
    request.usage.output_tokens = static_cast<int>(output_tokens);
    request.usage.cache_read_tokens = static_cast<int>(cached_tokens);
    request.usage.cache_creation_1h_tokens = 0; // OpenAI has no 1h/5m split
    request.usage.cache_creation_5m_tokens = static_cast<int>(cache_write_tokens);
    const json meta = response.is_object() ? response : root;
    if (const auto tier = meta["service_tier"].as_string(); tier.has_value()) {
        request.upstream.service_tier = normalize_usage_service_tier(std::string_view{ *tier });
    }
    if (const auto model = meta["model"].as_string(); model.has_value() && !model->empty()) {
        request.upstream.model_name = *model;
    }
}

namespace
{

// Official: response.service_tier is actual tier; long-context >272k is 2x for openai-owned.
// Priority unsupported for long context — do not invent 4x.
double tier_multiplier_for(std::string_view response_tier, int official_input_tokens, bool openai_owned)
{
    const std::string_view tier = trim_ascii(response_tier);
    if (tier == "priority") {
        return 2.0;
    }
    if (openai_owned && official_input_tokens > 272000) {
        return 2.0;
    }
    return 1.0;
}

} // namespace

bool OpenaiResponses::channel_ok(const Channel &channel) const
{
    return channel.status && channel.type == "openai_compatible" && !trim_ascii(channel.api_key).empty();
}

GatewayStreamKind OpenaiResponses::kind() const
{
    return GatewayStreamKind::openai_responses;
}

std::string_view OpenaiResponses::no_available_channel_message() const
{
    return "no available openai-compatible channel";
}

std::string_view OpenaiResponses::upstream_path() const
{
    return request.http.path;
}

UpstreamRequest OpenaiResponses::make_upstream(bool stream) const
{
    // Body already carries service_tier to upstream; no synthetic protocol headers.
    UpstreamRequest downstream;
    downstream.method = request.http.method;
    downstream.path = request.http.path;
    downstream.body = request.http.body;
    downstream.headers = {
        { "Content-Type", "application/json" },
        { "Accept", stream ? "text/event-stream" : "application/json" },
        { "X-Request-Id", request.request_id },
    };
    return downstream;
}

void OpenaiResponses::fill_success_pricing(ProxyRequest &pr, const Channel &channel)
{
    Gateway::fill_success_pricing(pr, channel);
    const Model *model = channel.find_model(pr.upstream.model_name);
    pr.upstream.tier_multiplier = tier_multiplier_for(pr.upstream.service_tier,
                                                      pr.usage.input_tokens + pr.usage.cache_read_tokens,
                                                      model != nullptr && model->owned_by == "openai");
}

bool OpenaiResponses::should_bill_non_stream() const
{
    return request.http.path != "/v1/responses/input_tokens";
}

bool OpenaiResponses::prepare(::httplib::Response &res)
{
    if (request.http.method == "POST") {
        return true;
    }
    write_upstream(res, 405, serialize(json{ { "error", json{ { "message", "method not allowed" } } } }),
                   { { "X-Request-Id", request.request_id }, { "Content-Type", "application/json; charset=utf-8" } });
    return false;
}

ResponsesProxyResult handle_responses_proxy_request(ProxyRequest &pr, ::httplib::Response &res)
{
    return handle_responses_proxy_request(pr, res, ResponsesProxyExecuteOptions{});
}

ResponsesProxyResult handle_responses_proxy_request(ProxyRequest &pr, ::httplib::Response &res,
                                                    const ResponsesProxyExecuteOptions &options)
{
    return OpenaiResponses(pr).handle(res, options);
}

} // namespace revlm
