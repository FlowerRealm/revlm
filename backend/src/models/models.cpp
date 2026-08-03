#include "models/catalog.hpp"

#include "plugins/runtime.hpp"

namespace revlm
{

std::vector<Model> models_for_channel(std::string_view channel_type)
{
    return plugin::models_for_channel_type(channel_type);
}

std::vector<Model> all_known_models()
{
    return plugin::all_plugin_models();
}

} // namespace revlm
