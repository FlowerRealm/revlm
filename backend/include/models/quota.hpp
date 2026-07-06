#pragma once

#include "request/request.hpp"
#include "store/mysql.hpp"

namespace revlm
{
class Quota {
public:
    explicit Quota(MysqlConnection &conn);
    void charge(Request request);

private:
    MysqlConnection &conn_;
};

} // namespace revlm
