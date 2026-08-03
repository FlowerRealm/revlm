#pragma once

#include <string_view>

#include "util/json.hpp"

namespace revlm::plugin
{

json admin_plugins_response(std::string_view raw_request, std::string *set_cookie = nullptr);
json admin_plugin_upload_response(std::string_view raw_request, std::string_view filename, std::string_view archive,
                                  std::string *set_cookie = nullptr);
json admin_plugin_enable_response(std::string_view raw_request, std::string_view plugin_id, bool enabled,
                                  std::string *set_cookie = nullptr);
json admin_plugin_uninstall_response(std::string_view raw_request, std::string_view plugin_id,
                                     std::string *set_cookie = nullptr);
json plugin_channel_types_response();

} // namespace revlm::plugin
