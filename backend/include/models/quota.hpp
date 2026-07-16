#pragma once

#include "request/request.hpp"
#include "store/database.hpp"

namespace revlm
{
class Quota {
public:
    Quota();
    void charge(Request request);

private:
    odb::database &db_;
};

} // namespace revlm
