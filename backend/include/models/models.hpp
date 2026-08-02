#pragma once

#include <string>
#include <utility>
#include <vector>

namespace revlm
{

class Model {
public:
    Model()
    {
    }
    Model(int id, std::string name, std::string owned_by, double input_price, double output_price,
          double cache_read_price, double cache_creation_1h_price, double cache_creation_5m_price,
          std::string icon_url = {})
        : id(id)
        , name(std::move(name))
        , owned_by(std::move(owned_by))
        , input_price(input_price)
        , output_price(output_price)
        , cache_read_price(cache_read_price)
        , cache_creation_1h_price(cache_creation_1h_price)
        , cache_creation_5m_price(cache_creation_5m_price)
        , icon_url(std::move(icon_url))
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
    std::string icon_url;
};

} // namespace revlm
