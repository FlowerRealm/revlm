#include "models/catalog.hpp"
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

revlm::json pricing_for(revlm::json input_price, revlm::json output_price, revlm::json cache_read_price,
                        revlm::json cache_creation_1h_price, revlm::json cache_creation_5m_price, revlm::json icon_url)
{
    revlm::json p;
    p["input_price"] = std::move(input_price);
    p["output_price"] = std::move(output_price);
    p["cache_read_price"] = std::move(cache_read_price);
    p["cache_creation_1h_price"] = std::move(cache_creation_1h_price);
    p["cache_creation_5m_price"] = std::move(cache_creation_5m_price);
    p["icon_url"] = std::move(icon_url);
    return p;
}

std::vector<revlm::Model> make_openai_catalog()
{
    return {
        revlm::Model(101, "gpt-5.5", pricing_for(5, 30, 0.5, 0, 0, "/assets/model-icons/openai.svg")),
        revlm::Model(102, "gpt-5.4", pricing_for(2.5, 15, 0.25, 0, 0, "/assets/model-icons/openai.svg")),
        revlm::Model(103, "gpt-5.4-mini", pricing_for(0.75, 4.5, 0.075, 0, 0, "/assets/model-icons/openai.svg")),
        revlm::Model(104, "gpt-5.3-codex", pricing_for(1.75, 14, 0.175, 0, 0, "/assets/model-icons/openai.svg")),
        revlm::Model(105, "codex-auto-review", pricing_for(2.5, 15, 0.25, 0, 0, "/assets/model-icons/openai.svg")),
    };
}

std::vector<revlm::Model> make_anthropic_catalog()
{
    return {
        revlm::Model(201, "claude-opus-4-8", pricing_for(5, 25, 0.5, 10, 6.25, "/assets/model-icons/claude-color.svg")),
        revlm::Model(202, "claude-opus-4-7", pricing_for(5, 25, 0.5, 10, 6.25, "/assets/model-icons/claude-color.svg")),
        revlm::Model(203, "claude-opus-4-6", pricing_for(5, 25, 0.5, 10, 6.25, "/assets/model-icons/claude-color.svg")),
        revlm::Model(204, "claude-haiku-4-5-20251001",
                     pricing_for(1, 5, 0.1, 2, 1.25, "/assets/model-icons/claude-color.svg")),
        revlm::Model(205, "claude-sonnet-4-6",
                     pricing_for(3, 15, 0.3, 6, 3.75, "/assets/model-icons/claude-color.svg")),
        revlm::Model(206, "claude-sonnet-5", pricing_for(2, 10, 0.2, 4, 3.75, "/assets/model-icons/claude-color.svg")),
    };
}

const revlm::Model *find_by_name(const std::vector<revlm::Model> &models, std::string_view name)
{
    const auto it = std::find_if(models.begin(), models.end(), [&](const revlm::Model &m) { return m.name == name; });
    return it == models.end() ? nullptr : &(*it);
}

} // namespace

int main()
{
    const std::vector<revlm::Model> openai_models = make_openai_catalog();
    const std::vector<revlm::Model> anthropic_models = make_anthropic_catalog();
    const std::vector<revlm::Model> unknown_models;

    if (expect(openai_models.size() == 5, "openai_compatible channel should expose 5 models") != 0 ||
        expect(anthropic_models.size() == 6, "anthropic channel should expose 6 models") != 0 ||
        expect(unknown_models.empty(), "unknown channel type should expose no models") != 0 ||
        expect(openai_models.front().name == "gpt-5.5", "openai models should preserve order") != 0 ||
        expect(anthropic_models.back().name == "claude-sonnet-5", "anthropic models should include latest") != 0 ||
        expect(anthropic_models[0].pricing["cache_creation_5m_price"].as_double().value_or(0) == 6.25,
               "anthropic cache creation price should be retained") != 0 ||
        expect(anthropic_models[0].pricing["cache_creation_1h_price"].as_double().value_or(0) == 10 &&
                   anthropic_models[3].pricing["cache_creation_1h_price"].as_double().value_or(0) == 2 &&
                   anthropic_models[4].pricing["cache_creation_1h_price"].as_double().value_or(0) == 6,
               "anthropic 1h cache creation prices should be retained") != 0 ||
        expect(openai_models.front().pricing["icon_url"].as_string().value_or("") == "/assets/model-icons/openai.svg",
               "openai plugin should own its model icon metadata") != 0 ||
        expect(anthropic_models.front().pricing["icon_url"].as_string().value_or("") ==
                   "/assets/model-icons/claude-color.svg",
               "anthropic plugin should own its model icon metadata") != 0) {
        return 1;
    }

    if (expect(find_by_name(openai_models, "codex-auto-review") != nullptr &&
                   find_by_name(openai_models, "codex-auto-review")->name == "codex-auto-review",
               "lookup should find openai model") != 0 ||
        expect(find_by_name(openai_models, "claude-opus-4-8") == nullptr,
               "openai catalog should not find anthropic model") != 0 ||
        expect(find_by_name(anthropic_models, "claude-opus-4-8") != nullptr &&
                   find_by_name(anthropic_models, "claude-opus-4-8")->name == "claude-opus-4-8",
               "lookup should find anthropic model") != 0 ||
        expect(find_by_name(openai_models, "missing") == nullptr, "lookup should miss unknown model") != 0) {
        return 1;
    }

    return 0;
}
