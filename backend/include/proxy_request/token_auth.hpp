#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <httplib.h>

#include "server/tokens.hpp"

namespace revlm
{

struct TokenAuthResult {
    std::optional<TokenAuth> auth;
    int status = 200;
    std::string message;
};

TokenAuthResult authenticated_token(const ::httplib::Request &req);
TokenAuthResult authenticated_token(std::string_view raw_request);

} // namespace revlm
