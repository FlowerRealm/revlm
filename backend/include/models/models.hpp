#pragma once

#include <string>
#include <vector>

namespace revlm
{

class Model {
public:
    Model()
    {
    }
    Model(int id, std::string name, std::string owned_by, double input_price, double output_price,
          double cache_read_price, double cache_creation_1h_price, double cache_creation_5m_price)
        : id(id)
        , name(std::move(name))
        , owned_by(std::move(owned_by))
        , input_price(input_price)
        , output_price(output_price)
        , cache_read_price(cache_read_price)
        , cache_creation_1h_price(cache_creation_1h_price)
        , cache_creation_5m_price(cache_creation_5m_price)
    {
    }
    int id = 0;
    std::string name;
    std::string owned_by;
    double input_price = 0;
    double output_price = 0;
    double cache_read_price = 0;
    double cache_creation_1h_price = 0;
    double cache_creation_5m_price = 0;
};

extern const Model GPT_5_5;
extern const Model GPT_5_4;
extern const Model GPT_5_4_MINI;
extern const Model GPT_5_3_CODEX;
extern const Model CODEX_AUTO_REVIEW;
extern const Model CLAUDE_OPUS_4_8;
extern const Model CLAUDE_OPUS_4_7;
extern const Model CLAUDE_OPUS_4_6;
extern const Model CLAUDE_HAIKU_4_5_20251001;
extern const Model CLAUDE_SONNET_4_6;
extern const Model CLAUDE_SONNET_5;

extern const std::vector<Model> all_models;

} // namespace revlm
