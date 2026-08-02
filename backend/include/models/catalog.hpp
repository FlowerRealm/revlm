#pragma once

#include <string_view>
#include <vector>

#include "models/models.hpp"

namespace revlm
{

// These are ordinary interposable functions, not a provider registry. A
// module may replace them, chain to the next definition, or ignore them and
// replace a wider piece of the application instead.
extern "C" void revlm_models_for_channel_type(std::string_view channel_type, std::vector<Model> &models);
extern "C" void revlm_all_models(std::vector<Model> &models);

std::vector<Model> models_for_channel(std::string_view channel_type);
std::vector<Model> all_known_models();

} // namespace revlm
