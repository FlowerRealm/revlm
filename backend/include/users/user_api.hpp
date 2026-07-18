#pragma once

#include <optional>
#include <string_view>

#include "server/http_server.hpp"
#include "users/users.hpp"

namespace revlm
{

HttpResponse register_response(std::string_view raw_request, std::string_view body, std::string_view request_id);
HttpResponse login_response(std::string_view raw_request, std::string_view request_id, std::string_view body);
HttpResponse self_response(std::string_view raw_request, std::string_view request_id);
HttpResponse logout_response(std::string_view raw_request, std::string_view request_id);
HttpResponse account_email_response(std::string_view raw_request, std::string_view body, std::string_view request_id);
HttpResponse account_password_response(std::string_view raw_request, std::string_view body,
                                       std::string_view request_id);
std::optional<User> api_authenticated_user(std::string_view raw_request, std::string_view request_id,
                                           HttpResponse &response);

} // namespace revlm
