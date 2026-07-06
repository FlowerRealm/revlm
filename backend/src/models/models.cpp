#include "models/models.hpp"

namespace revlm
{

const ModelManager &ModelManager::instance()
{
    static const ModelManager manager;
    return manager;
}

ModelManager::ModelManager()
{
    models_.push_back(Model(101, "gpt-5.5", "openai", 5, 30, 0.5, 0, 0));
    models_.push_back(Model(102, "gpt-5.4", "openai", 2.5, 15, 0.25, 0, 0));
    models_.push_back(Model(103, "gpt-5.4-mini", "openai", 0.75, 4.5, 0.075, 0, 0));
    models_.push_back(Model(104, "gpt-5.3-codex", "openai", 1.75, 14, 0.175, 0, 0));
    models_.push_back(Model(105, "codex-auto-review", "openai", 2.5, 15, 0.25, 0, 0));
    models_.push_back(Model(201, "claude-opus-4-8", "anthropic", 5, 25, 0.5, 10, 6.25));
    models_.push_back(Model(202, "claude-opus-4-7", "anthropic", 5, 25, 0.5, 10, 6.25));
    models_.push_back(Model(203, "claude-opus-4-6", "anthropic", 5, 25, 0.5, 10, 6.25));
    models_.push_back(Model(204, "claude-haiku-4-5-20251001", "anthropic", 1, 5, 0.1, 2, 1.25));
    models_.push_back(Model(205, "claude-sonnet-4-6", "anthropic", 3, 15, 0.3, 6, 3.75));
    models_.push_back(Model(206, "claude-sonnet-5", "anthropic", 2, 10, 0.2, 4, 2.5));
}

} // namespace revlm
