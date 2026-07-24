#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "util/json.hpp"
#include "users/users.hpp"

namespace revlm
{

json admin_list_users_response(std::string_view raw_request, std::string *set_cookie = nullptr);
json admin_create_user_response(std::string_view raw_request, std::string_view body, std::string *set_cookie = nullptr);
json admin_update_user_response(long long user_id, std::string_view raw_request, std::string_view body,
                                std::string *set_cookie = nullptr);
json admin_reset_user_password_response(long long user_id, std::string_view raw_request, std::string_view body,
                                        std::string *set_cookie = nullptr);
json admin_add_user_balance_response(long long user_id, std::string_view raw_request, std::string_view body,
                                     std::string *set_cookie = nullptr);
json admin_delete_user_response(long long user_id, std::string_view raw_request, std::string *set_cookie = nullptr);

} // namespace revlm
