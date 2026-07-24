#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <httplib.h>

#include "users/users.hpp"
#include "util/json.hpp"

namespace revlm
{

std::optional<std::string> extract_api_token(const httplib::Request &req);
std::optional<long long> authenticate_api_token(const httplib::Request &req, long long &user_id, long long &token_id);

json list_user_tokens_response(const User &user);
json create_user_token_response(std::string_view raw_request, std::string_view body, std::string *set_cookie);
json reveal_user_token_response(std::string_view raw_request, long long token_id, std::string *set_cookie);
json rotate_user_token_response(std::string_view raw_request, long long token_id, std::string *set_cookie);
json revoke_user_token_response(std::string_view raw_request, long long token_id, std::string *set_cookie);
json delete_user_token_response(std::string_view raw_request, long long token_id, std::string *set_cookie);
json token_channel_response(std::string_view raw_request, long long token_id, std::string *set_cookie);
json set_token_channel_response(std::string_view raw_request, long long token_id, std::string_view body,
                                std::string *set_cookie);

} // namespace revlm
