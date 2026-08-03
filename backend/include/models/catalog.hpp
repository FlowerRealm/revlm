#pragma once

#include <string_view>
#include <vector>

#include "models/models.hpp"

namespace revlm
{

std::vector<Model> models_for_channel(std::string_view channel_type);
std::vector<Model> all_known_models();

} // namespace revlm
