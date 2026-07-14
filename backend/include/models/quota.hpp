#pragma once

#include "request/request.hpp"
#include "store/database.hpp"

namespace revlm
{
class Quota {
public:
    explicit Quota(odb::database &db);
    void charge(Request request);

private:
    odb::database &db_;
};

} // namespace revlm
