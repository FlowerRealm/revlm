#include "channels/channels.hpp"
#include "models/models.hpp"

#include <iostream>

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

} // namespace

int main()
{
    const revlm::Channel openai(0, "openai_compatible", "", true, 0, "");
    const revlm::Channel anthropic(0, "anthropic", "", true, 0, "");
    const revlm::Channel unknown(0, "other", "", true, 0, "");

    if (expect(openai.models.size() == 5, "openai_compatible channel should expose 5 models") != 0 ||
        expect(anthropic.models.size() == 6, "anthropic channel should expose 6 models") != 0 ||
        expect(unknown.models.empty(), "unknown channel type should expose no models") != 0 ||
        expect(openai.models.front().name == "gpt-5.5", "openai models should preserve order") != 0 ||
        expect(anthropic.models.back().name == "claude-sonnet-5", "anthropic models should include latest") != 0 ||
        expect(anthropic.models[0].cache_creation_5m_price == 6.25,
               "anthropic cache creation price should be retained") != 0 ||
        expect(anthropic.models[0].cache_creation_1h_price == 10 && anthropic.models[3].cache_creation_1h_price == 2 &&
                   anthropic.models[4].cache_creation_1h_price == 6,
               "anthropic 1h cache creation prices should be retained") != 0) {
        return 1;
    }

    if (expect(openai.find_model("codex-auto-review") != nullptr &&
                   openai.find_model("codex-auto-review")->name == "codex-auto-review",
               "lookup should find openai model") != 0 ||
        expect(openai.find_model("claude-opus-4-8") == nullptr, "openai channel should not find anthropic model") !=
            0 ||
        expect(anthropic.find_model("claude-opus-4-8") != nullptr &&
                   anthropic.find_model("claude-opus-4-8")->name == "claude-opus-4-8",
               "lookup should find anthropic model") != 0 ||
        expect(openai.find_model("missing") == nullptr, "lookup should miss unknown model") != 0) {
        return 1;
    }

    return 0;
}
