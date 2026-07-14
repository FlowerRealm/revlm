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
        , name(name)
        , owned_by(owned_by)
        , input_price(input_price)
        , output_price(output_price)
        , cache_read_price(cache_read_price)
        , cache_creation_1h_price(cache_creation_1h_price)
        , cache_creation_5m_price(cache_creation_5m_price)
    {
    }
    int id;
    std::string name;
    std::string owned_by;
    double input_price;
    double output_price;
    double cache_read_price;
    double cache_creation_1h_price;
    double cache_creation_5m_price;

private:
};
class ModelManager {
public:
    static const ModelManager &instance();
    const std::vector<Model> &models() const
    {
        return models_;
    }

private:
    ModelManager();
    std::vector<Model> models_;
};

} // namespace revlm
