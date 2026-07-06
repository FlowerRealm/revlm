#include "proxy_request/upstream.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

int expect(bool ok, const char *message)
{
    if (ok) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

std::string header_value(const std::vector<revlm::UpstreamHeader> &headers, std::string_view name)
{
    for (const auto &header : headers) {
        std::string left = header.name;
        std::string right = std::string{ name };
        for (char &ch : left) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        for (char &ch : right) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        if (left == right) {
            return header.value;
        }
    }
    return {};
}

revlm::SchedulerSelection make_openai_selection()
{
    revlm::SchedulerSelection selection{};
    selection.channel_id = 1;
    selection.channel_type = "openai_compatible";
    selection.base_url = "https://api.example.test/v1";
    selection.api_key = "sk-openai";
    selection.openai_organization = "org_123";
    return selection;
}

revlm::SchedulerSelection make_anthropic_selection()
{
    revlm::SchedulerSelection selection{};
    selection.channel_id = 2;
    selection.channel_type = "anthropic";
    selection.base_url = "https://claude.example.test";
    selection.api_key = "sk-anthropic";
    return selection;
}

} // namespace

int main()
{
    revlm::UpstreamExecutor executor;

    {
        auto blocked = make_openai_selection();
        blocked.base_url = "http://127.0.0.1:18080";
        revlm::UpstreamRequest request;
        request.method = "POST";
        request.path = "/v1/responses";
        request.body = R"({"model":"gpt-5"})";
        bool rejected = false;
        try {
            (void)executor.prepare(blocked, request);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        if (expect(rejected, "executor should reject blocked SSRF upstream targets") != 0) {
            return 1;
        }
    }

    {
        revlm::UpstreamRequest request{
            .method = "POST",
            .path = "/v1/responses",
            .query = "stream=true",
            .headers = { { "Authorization", "Bearer user" }, { "Accept-Encoding", "gzip" }, { "X-Test", "ok" } },
            .body = R"({"model":"gpt-5","max_output_tokens":32,"stream":true})",
        };
        const auto prepared = executor.prepare(make_openai_selection(), request);
        if (expect(prepared.url == "https://api.example.test/v1/responses?stream=true",
                   "openai base /v1 should collapse downstream /v1 prefix") != 0 ||
            expect(header_value(prepared.headers, "authorization") == "Bearer sk-openai",
                   "openai executor should inject upstream bearer token") != 0 ||
            expect(header_value(prepared.headers, "openai-organization") == "org_123",
                   "openai executor should inject org header") != 0 ||
            expect(header_value(prepared.headers, "accept-encoding") == "identity",
                   "openai executor should force identity encoding") != 0 ||
            expect(header_value(prepared.headers, "x-test") == "ok", "openai executor should preserve safe headers") !=
                0) {
            return 1;
        }
    }

    {
        revlm::UpstreamRequest request{
            .method = "POST",
            .path = "/v1/messages",
            .query = {},
            .headers = {},
            .body = R"({"model":"claude","stream":false})",
        };
        const auto prepared = executor.prepare(make_anthropic_selection(), request);
        if (expect(prepared.url == "https://claude.example.test/v1/messages",
                   "anthropic executor should keep message path") != 0 ||
            expect(header_value(prepared.headers, "x-api-key") == "sk-anthropic",
                   "anthropic executor should inject x-api-key") != 0 ||
            expect(header_value(prepared.headers, "anthropic-version") == "2023-06-01",
                   "anthropic executor should default version header") != 0) {
            return 1;
        }
    }

    {
        revlm::UpstreamRequest request{
            .method = "POST",
            .path = "/v1/responses",
            .query = {},
            .headers = {},
            .body = R"({"model":"gpt-5","max_output_tokens":16,"stream_options":{"include_usage":true}})",
        };
        int calls = 0;
        const auto result = executor.execute(
            make_openai_selection(), request,
            [&](const revlm::UpstreamPreparedRequest &prepared) -> revlm::UpstreamResponse {
                ++calls;
                if (calls == 1) {
                    if (expect(prepared.body.find("\"max_output_tokens\"") != std::string::npos,
                               "first request should preserve original body before retry") != 0) {
                        throw std::runtime_error("first body mismatch");
                    }
                    return revlm::UpstreamResponse{
                        .status_code = 400,
                        .headers = {},
                        .body = R"({"error":{"message":"Unsupported parameter: max_output_tokens"}})",
                    };
                }
                if (expect(prepared.retried_unsupported_parameter,
                           "retry should be marked as unsupported-parameter rewrite") != 0 ||
                    expect(prepared.body.find("\"max_tokens\":16") != std::string::npos,
                           "retry should rewrite max_output_tokens to max_tokens") != 0 ||
                    expect(prepared.body.find("\"stream_options\"") != std::string::npos,
                           "retry should keep unrelated fields when rewriting another field") != 0) {
                    throw std::runtime_error("retry body mismatch");
                }
                return revlm::UpstreamResponse{ .status_code = 200, .headers = {}, .body = R"({"ok":true})" };
            });
        if (expect(calls == 2, "executor should retry exactly once on unsupported parameter") != 0 ||
            expect(result.rewrote_unsupported_parameter, "executor should report unsupported parameter rewrite") != 0 ||
            expect(result.response.status_code == 200, "retry response should replace original 4xx") != 0) {
            return 1;
        }
    }

    {
        revlm::UpstreamRequest request{
            .method = "POST",
            .path = "/v1/responses",
            .query = {},
            .headers = {},
            .body = R"({"model":"gpt-5","max_tokens":16})",
        };
        int calls = 0;
        const auto result = executor.execute(
            make_openai_selection(), request, [&](const revlm::UpstreamPreparedRequest &) -> revlm::UpstreamResponse {
                ++calls;
                if (calls == 1) {
                    return revlm::UpstreamResponse{
                        .status_code = 400,
                        .headers = {},
                        .body = R"({"error":{"message":"Unsupported parameter: unrelated"}})",
                    };
                }
                return revlm::UpstreamResponse{ .status_code = 200, .headers = {}, .body = R"({"ok":true})" };
            });
        if (expect(calls == 1, "executor should not retry when parameter is not supported by rewrite list") != 0 ||
            expect(!result.rewrote_unsupported_parameter, "executor should leave unrelated 4xx untouched") != 0 ||
            expect(result.response.status_code == 400,
                   "executor should preserve original 4xx when no rewrite applies") != 0) {
            return 1;
        }
    }

    {
        revlm::UpstreamRequest request{
            .method = "POST",
            .path = "/v1/responses",
            .query = {},
            .headers = {},
            .body = R"({"model":"gpt-5","max_output_tokens":16})",
        };
        int calls = 0;
        const auto result = executor.execute(
            make_openai_selection(), request, [&](const revlm::UpstreamPreparedRequest &) -> revlm::UpstreamResponse {
                ++calls;
                if (calls == 1) {
                    return revlm::UpstreamResponse{
                        .status_code = 400,
                        .headers = {},
                        .body = R"({"error":{"message":"Unsupported parameter: max_output_tokens"}})",
                    };
                }
                return revlm::UpstreamResponse{
                    .status_code = 400,
                    .headers = {},
                    .body = R"({"error":{"message":"still bad"}})",
                };
            });
        if (expect(calls == 2, "executor should attempt one rewrite retry on supported parameter") != 0 ||
            expect(!result.rewrote_unsupported_parameter,
                   "executor should keep original 4xx when rewrite retry also fails") != 0 ||
            expect(result.response.status_code == 400,
                   "executor should preserve first 4xx when rewrite retry is unsuccessful") != 0) {
            return 1;
        }
    }

    return 0;
}
