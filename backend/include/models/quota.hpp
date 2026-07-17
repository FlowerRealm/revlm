#pragma once

#include "request/request.hpp"
#include <odb/database.hxx>

namespace revlm
{
class Quota {
public:
    Quota();
    void charge(const Request &request);

private:
    odb::database &db_;
};

} // namespace revlm
