#pragma once

#include <string>
#include <utility>

#include "util/json.hpp"

namespace revlm
{

// Shared Model ABI is deliberately minimal: only the model identity and the
// plugin-owned pricing JSON survive. Fixed per-token price fields, owned_by and
// icon_url are removed; the model catalog is owned by the plugin and indexed by
// ChannelGroup.type, so the core never parses pricing.
class Model {
public:
    Model()
    {
    }
    Model(int id, std::string name, json pricing)
        : id(id)
        , name(std::move(name))
        , pricing(std::move(pricing))
    {
    }
    int id = 0;
    std::string name;
    json pricing;
};

} // namespace revlm
