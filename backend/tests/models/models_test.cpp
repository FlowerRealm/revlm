#include "models/models.hpp"

#include <algorithm>
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
    const std::vector<revlm::Model> &models = revlm::ModelManager::instance().models();
    if (expect(models.size() == 11, "builtin catalog should contain 11 models") != 0 ||
        expect(models.front().name == "gpt-5.5", "builtin catalog should preserve order") != 0 ||
        expect(models.back().name == "claude-sonnet-5", "builtin catalog should include latest anthropic model") != 0 ||
        expect(models[5].cache_creation_5m_price == 6.25, "anthropic cache creation price should be retained") != 0 ||
        expect(models[5].cache_creation_1h_price == 10 && models[8].cache_creation_1h_price == 2 &&
                   models[9].cache_creation_1h_price == 6,
               "anthropic 1h cache creation prices should be retained") != 0) {
        return 1;
    }

    const auto codex_it = std::ranges::find(models, std::string{ "codex-auto-review" }, &revlm::Model::name);
    const auto missing_it = std::ranges::find(models, std::string{ "missing" }, &revlm::Model::name);
    if (expect(codex_it != models.end() && codex_it->owned_by == "openai", "lookup should find builtin model") != 0 ||
        expect(missing_it == models.end(), "lookup should miss unknown model") != 0) {
        return 1;
    }

    return 0;
}
