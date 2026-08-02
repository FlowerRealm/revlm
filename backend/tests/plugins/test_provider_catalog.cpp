#include "channels/channels.hpp"
#include "models/catalog.hpp"
#include "proxy/upstream.hpp"
#include "util/json.hpp"
#include "util/strings.hpp"

#include <algorithm>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using revlm::Channel;
using revlm::Model;
using revlm::UpstreamHeader;
using revlm::UpstreamPreparedRequest;
using revlm::UpstreamRequest;
using revlm::UpstreamResponse;
using revlm::json;

std::vector<Model> openai_catalog()
{
    return {
        Model(101, "gpt-5.5", "openai", 5, 30, 0.5, 0, 0, "/assets/model-icons/openai.svg"),
        Model(102, "gpt-5.4", "openai", 2.5, 15, 0.25, 0, 0, "/assets/model-icons/openai.svg"),
        Model(103, "gpt-5.4-mini", "openai", 0.75, 4.5, 0.075, 0, 0, "/assets/model-icons/openai.svg"),
        Model(104, "gpt-5.3-codex", "openai", 1.75, 14, 0.175, 0, 0, "/assets/model-icons/openai.svg"),
        Model(105, "codex-auto-review", "openai", 2.5, 15, 0.25, 0, 0, "/assets/model-icons/openai.svg"),
    };
}

std::vector<Model> anthropic_catalog()
{
    return {
        Model(201, "claude-opus-4-8", "anthropic", 5, 25, 0.5, 10, 6.25, "/assets/model-icons/claude-color.svg"),
        Model(202, "claude-opus-4-7", "anthropic", 5, 25, 0.5, 10, 6.25, "/assets/model-icons/claude-color.svg"),
        Model(203, "claude-opus-4-6", "anthropic", 5, 25, 0.5, 10, 6.25, "/assets/model-icons/claude-color.svg"),
        Model(204, "claude-haiku-4-5-20251001", "anthropic", 1, 5, 0.1, 2, 1.25,
              "/assets/model-icons/claude-color.svg"),
        Model(205, "claude-sonnet-4-6", "anthropic", 3, 15, 0.3, 6, 3.75, "/assets/model-icons/claude-color.svg"),
        Model(206, "claude-sonnet-5", "anthropic", 2, 10, 0.2, 4, 3.75, "/assets/model-icons/claude-color.svg"),
    };
}

bool header_matches(const UpstreamHeader &header, std::string_view name)
{
    return revlm::lowercase_ascii(header.name) == revlm::lowercase_ascii(name);
}

void erase_header(std::vector<UpstreamHeader> &headers, std::string_view name)
{
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [&](const UpstreamHeader &header) { return header_matches(header, name); }),
                  headers.end());
}

std::string header_value(const std::vector<UpstreamHeader> &headers, std::string_view name)
{
    const auto it = std::find_if(headers.begin(), headers.end(),
                                 [&](const UpstreamHeader &header) { return header_matches(header, name); });
    return it == headers.end() ? "" : it->value;
}

void set_header(std::vector<UpstreamHeader> &headers, std::string_view name, std::string value)
{
    const auto it = std::find_if(headers.begin(), headers.end(),
                                 [&](const UpstreamHeader &header) { return header_matches(header, name); });
    if (it != headers.end()) {
        it->name = std::string{ name };
        it->value = std::move(value);
        return;
    }
    headers.push_back({ std::string{ name }, std::move(value) });
}

void prepare_common_headers(UpstreamPreparedRequest &prepared)
{
    erase_header(prepared.headers, "Authorization");
    erase_header(prepared.headers, "X-Api-Key");
    erase_header(prepared.headers, "Accept-Encoding");
    set_header(prepared.headers, "Accept-Encoding", "identity");
}

std::string unsupported_parameter_name(std::string_view body)
{
    static const std::regex pattern("unsupported parameter[^a-z0-9_]+([a-z0-9_]+)", std::regex_constants::icase);
    std::smatch match;
    const std::string haystack{ body };
    if (std::regex_search(haystack, match, pattern) && match.size() >= 2) {
        return revlm::lowercase_ascii(match[1].str());
    }
    return {};
}

bool rewrite_body_field(std::string_view body, std::string_view source_name, std::string_view destination_name,
                        bool keep_destination, std::string &out)
{
    auto doc = json::parse(body);
    if (!doc || !doc->is_object() || !doc->contains(source_name)) {
        return false;
    }
    json value = static_cast<const json &>(*doc)[source_name];
    doc->erase(source_name);
    if (!keep_destination || !doc->contains(destination_name)) {
        (*doc)[destination_name] = std::move(value);
    }
    out = doc->dump();
    return true;
}

bool remove_body_field(std::string_view body, std::string_view name, std::string &out)
{
    auto doc = json::parse(body);
    if (!doc || !doc->is_object() || !doc->contains(name)) {
        return false;
    }
    doc->erase(name);
    out = doc->dump();
    return true;
}

} // namespace

namespace revlm
{

extern "C" void revlm_models_for_channel_type(std::string_view channel_type, std::vector<Model> &models)
{
    if (channel_type == "openai_compatible") {
        models = openai_catalog();
        return;
    }
    if (channel_type == "anthropic") {
        models = anthropic_catalog();
        return;
    }
    models.clear();
}

extern "C" void revlm_all_models(std::vector<Model> &models)
{
    models = openai_catalog();
    const auto anthropic = anthropic_catalog();
    models.insert(models.end(), anthropic.begin(), anthropic.end());
}

extern "C" void revlm_prepare_upstream(const Channel &channel, const UpstreamRequest &downstream,
                                       UpstreamPreparedRequest &prepared)
{
    prepare_common_headers(prepared);
    if (channel.type == "openai_compatible") {
        set_header(prepared.headers, "Authorization", "Bearer " + channel.api_key);
        return;
    }
    if (channel.type == "anthropic") {
        if (downstream.path != "/v1/messages") {
            throw std::invalid_argument("anthropic upstream only supports /v1/messages");
        }
        if (revlm::trim_ascii(header_value(prepared.headers, "anthropic-version")).empty()) {
            set_header(prepared.headers, "anthropic-version", "2023-06-01");
        }
        set_header(prepared.headers, "x-api-key", channel.api_key);
        return;
    }
    throw std::runtime_error("no test provider for channel type: " + channel.type);
}

extern "C" bool revlm_retry_upstream_request(const Channel &channel, const UpstreamPreparedRequest &prepared,
                                             const UpstreamResponse &response, UpstreamPreparedRequest &retry)
{
    if (channel.type != "openai_compatible" || prepared.retried_unsupported_parameter || response.status_code < 400 ||
        response.status_code >= 500) {
        return false;
    }
    const std::string parameter = unsupported_parameter_name(response.body);
    std::string body;
    bool rewritten = false;
    if (parameter == "max_output_tokens") {
        rewritten = rewrite_body_field(prepared.body, "max_output_tokens", "max_tokens", true, body);
    } else if (parameter == "max_tokens") {
        rewritten = rewrite_body_field(prepared.body, "max_tokens", "max_output_tokens", false, body);
    } else if (parameter == "max_completion_tokens") {
        rewritten = rewrite_body_field(prepared.body, "max_completion_tokens", "max_tokens", true, body);
    } else if (parameter == "stream_options") {
        rewritten = remove_body_field(prepared.body, "stream_options", body);
    }
    if (!rewritten || body.empty() || body == prepared.body) {
        return false;
    }
    retry = prepared;
    retry.body = std::move(body);
    retry.retried_unsupported_parameter = true;
    return true;
}

} // namespace revlm
