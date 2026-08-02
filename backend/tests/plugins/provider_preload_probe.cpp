#include "channels/channels.hpp"
#include "models/catalog.hpp"
#include "proxy/upstream.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

bool has_header(const std::vector<revlm::UpstreamHeader> &headers, std::string_view name, std::string_view value)
{
    return std::any_of(headers.begin(), headers.end(), [&](const revlm::UpstreamHeader &header) {
        return header.name == name && header.value == value;
    });
}

bool has_model(const std::vector<revlm::Model> &models, std::string_view name)
{
    return std::any_of(models.begin(), models.end(), [&](const revlm::Model &model) { return model.name == name; });
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    const std::string_view provider{ argv[1] };
    if (provider == "openai") {
        revlm::Channel channel;
        channel.type = "openai_compatible";
        channel.api_key = "test-key";
        revlm::UpstreamRequest downstream;
        downstream.path = "/v1/chat/completions";
        downstream.body = "{\"max_output_tokens\":7}";
        revlm::UpstreamPreparedRequest prepared;
        prepared.body = downstream.body;
        try {
            revlm::revlm_prepare_upstream(channel, downstream, prepared);
        } catch (const std::exception &error) {
            std::cerr << "OpenAI preload failed: " << error.what() << '\n';
            return 1;
        }
        revlm::UpstreamResponse failed;
        failed.status_code = 400;
        failed.body = "unsupported parameter max_output_tokens";
        revlm::UpstreamPreparedRequest retry;
        std::vector<revlm::Model> models;
        revlm::revlm_models_for_channel_type(channel.type, models);
        if (!has_header(prepared.headers, "Authorization", "Bearer test-key") ||
            !revlm::revlm_retry_upstream_request(channel, prepared, failed, retry) ||
            retry.body.find("max_tokens") == std::string::npos || !has_model(models, "gpt-5.5")) {
            std::cerr << "OpenAI preload did not own the upstream behavior\n";
            return 1;
        }
        std::cout << "openai\n";
        return 0;
    }
    if (provider == "anthropic") {
        revlm::Channel channel;
        channel.type = "anthropic";
        channel.api_key = "test-key";
        revlm::UpstreamRequest downstream;
        downstream.path = "/v1/messages";
        revlm::UpstreamPreparedRequest prepared;
        try {
            revlm::revlm_prepare_upstream(channel, downstream, prepared);
        } catch (const std::exception &error) {
            std::cerr << "Anthropic preload failed: " << error.what() << '\n';
            return 1;
        }
        std::vector<revlm::Model> models;
        revlm::revlm_models_for_channel_type(channel.type, models);
        if (!has_header(prepared.headers, "x-api-key", "test-key") ||
            !has_header(prepared.headers, "anthropic-version", "2023-06-01") || !has_model(models, "claude-opus-4-8")) {
            std::cerr << "Anthropic preload did not construct headers\n";
            return 1;
        }
        std::cout << "anthropic\n";
        return 0;
    }
    return 2;
}
