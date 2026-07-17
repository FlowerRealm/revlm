#pragma once

#include <httplib.h>
#include <boost/json.hpp>

namespace revlm
{

boost::json::object authenticate_token(const ::httplib::Request &req);

} // namespace revlm
