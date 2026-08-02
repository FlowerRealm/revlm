#include "models/catalog.hpp"

namespace revlm
{

#ifndef REVLM_TEST_PROVIDER_CATALOG
extern "C" void revlm_models_for_channel_type(std::string_view, std::vector<Model> &models)
{
    models.clear();
}

extern "C" void revlm_all_models(std::vector<Model> &models)
{
    models.clear();
}
#endif

std::vector<Model> models_for_channel(std::string_view channel_type)
{
    std::vector<Model> models;
    revlm_models_for_channel_type(channel_type, models);
    return models;
}

std::vector<Model> all_known_models()
{
    std::vector<Model> models;
    revlm_all_models(models);
    return models;
}

} // namespace revlm
