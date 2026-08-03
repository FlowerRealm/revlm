#include "plugins/api.hpp"

#include "plugins/packages.hpp"
#include "plugins/runtime.hpp"
#include "users/user_api.hpp"
#include "util/strings.hpp"

#include <string>

namespace revlm::plugin
{
namespace
{

bool require_root(std::string_view raw_request, json &error, std::string *set_cookie)
{
    return api_authenticated_admin(raw_request, error, set_cookie).has_value();
}

json action_json(const PluginActionResult &result)
{
    json response;
    response["success"] = result.ok;
    response["message"] = result.message;
    return response;
}

} // namespace

json admin_plugins_response(std::string_view raw_request, std::string *set_cookie)
{
    json error;
    if (!require_root(raw_request, error, set_cookie)) {
        return error;
    }
    try {
        return json({ { "success", true }, { "data", plugin_installations_json() } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json admin_plugin_upload_response(std::string_view raw_request, std::string_view filename, std::string_view archive,
                                  std::string *set_cookie)
{
    json error;
    if (!require_root(raw_request, error, set_cookie)) {
        return error;
    }
    const std::string normalized_filename = trim_ascii(filename);
    if (!normalized_filename.ends_with(".revlm-plugin")) {
        return json({ { "success", false }, { "message", "只接受 .revlm-plugin ZIP 包" } });
    }
    return action_json(install_plugin_archive(archive));
}

json admin_plugin_enable_response(std::string_view raw_request, std::string_view plugin_id, bool enabled,
                                  std::string *set_cookie)
{
    json error;
    if (!require_root(raw_request, error, set_cookie)) {
        return error;
    }
    return action_json(set_plugin_enabled(plugin_id, enabled));
}

json admin_plugin_uninstall_response(std::string_view raw_request, std::string_view plugin_id, std::string *set_cookie)
{
    json error;
    if (!require_root(raw_request, error, set_cookie)) {
        return error;
    }
    return action_json(schedule_plugin_uninstall(plugin_id));
}

json plugin_channel_types_response()
{
    return json({ { "success", true }, { "data", plugin_channel_types_json() } });
}

} // namespace revlm::plugin
