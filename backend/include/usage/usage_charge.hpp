#pragma once

#include <string_view>

#include "request/request.hpp"
#include "store/mysql.hpp"

namespace revlm
{

void absorb_usage_object(Request &req, std::string_view body);

void charge_request(MysqlConnection &conn, Request &billing_request);

} // namespace revlm
