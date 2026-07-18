#include "models/models.hpp"

namespace revlm
{

const Model GPT_5_5(101, "gpt-5.5", "openai", 5, 30, 0.5, 0, 0);
const Model GPT_5_4(102, "gpt-5.4", "openai", 2.5, 15, 0.25, 0, 0);
const Model GPT_5_4_MINI(103, "gpt-5.4-mini", "openai", 0.75, 4.5, 0.075, 0, 0);
const Model GPT_5_3_CODEX(104, "gpt-5.3-codex", "openai", 1.75, 14, 0.175, 0, 0);
const Model CODEX_AUTO_REVIEW(105, "codex-auto-review", "openai", 2.5, 15, 0.25, 0, 0);
const Model CLAUDE_OPUS_4_8(201, "claude-opus-4-8", "anthropic", 5, 25, 0.5, 10, 6.25);
const Model CLAUDE_OPUS_4_7(202, "claude-opus-4-7", "anthropic", 5, 25, 0.5, 10, 6.25);
const Model CLAUDE_OPUS_4_6(203, "claude-opus-4-6", "anthropic", 5, 25, 0.5, 10, 6.25);
const Model CLAUDE_HAIKU_4_5_20251001(204, "claude-haiku-4-5-20251001", "anthropic", 1, 5, 0.1, 2, 1.25);
const Model CLAUDE_SONNET_4_6(205, "claude-sonnet-4-6", "anthropic", 3, 15, 0.3, 6, 3.75);
const Model CLAUDE_SONNET_5(206, "claude-sonnet-5", "anthropic", 2, 10, 0.2, 4, 2.5);

const std::vector<Model> all_models = {
    GPT_5_5,           GPT_5_4,           GPT_5_4_MINI,
    GPT_5_3_CODEX,     CODEX_AUTO_REVIEW, CLAUDE_OPUS_4_8,
    CLAUDE_OPUS_4_7,   CLAUDE_OPUS_4_6,   CLAUDE_HAIKU_4_5_20251001,
    CLAUDE_SONNET_4_6, CLAUDE_SONNET_5,
};

} // namespace revlm
