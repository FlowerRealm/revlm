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
    // V1 keeps provider catalogs in dynamically loaded plugins. This unit
    // test intentionally runs without a worker plugin snapshot.
    const revlm::Channel openai(0, "openai_compatible", "", true, 0, "");
    const revlm::Channel anthropic(0, "anthropic", "", true, 0, "");
    const revlm::Channel unknown(0, "other", "", true, 0, "");

    if (expect(openai.models.empty(), "unloaded openai plugin should expose no models") != 0 ||
        expect(anthropic.models.empty(), "unloaded anthropic plugin should expose no models") != 0 ||
        expect(unknown.models.empty(), "unknown channel type should expose no models") != 0 ||
        expect(openai.find_model("gpt-5.5") == nullptr, "unloaded plugin model lookup should be empty") != 0 ||
        expect(anthropic.find_model("claude-opus-4-8") == nullptr, "unloaded plugin model lookup should be empty") !=
            0) {
        return 1;
    }

    return 0;
}
