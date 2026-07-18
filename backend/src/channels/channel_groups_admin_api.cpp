#include "channels/channel_groups.hpp"

#include "auth/session.hpp"
#include "channels/channels.hpp"
#include "server/http_server.hpp"
#include "util/json_convert.hpp"
#include "util/json.hpp"
#include "util/json_util.hpp"
#include "util/user_input.hpp"

#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revlm
{
namespace
{

struct ParsedRequest {
    std::string_view method;
    std::string_view path;
    std::string_view target;
};

struct RootAuth {
    bool ok = false;
    bool clear_cookie = false;
    std::string failure;
};

RootAuth authenticate_root_admin(std::string_view raw_request)
{
    RootAuth out;
    const WebSessionAuth auth = authenticate_root_web_session(raw_request);
    out.clear_cookie = auth.clear_cookie;
    if (!auth.ok) {
        out.failure = auth.failure_message;
        return out;
    }
    out.ok = true;
    return out;
}

HttpResponse admin_auth_failure(std::string_view request_id, std::string_view message, bool clear_cookie,
                                std::string_view raw_request)
{
    std::vector<Header> headers{ { "X-Request-Id", std::string{ request_id } } };
    if (clear_cookie) {
        headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
    }
    return http_response(200, "OK", json({ { "success", false }, { "message", message } }), std::move(headers));
}

json channel_group_member_json(const Channel &channel)
{
    return json({ { "channel_id", channel.id },
                  { "name", channel.name },
                  { "type", channel.type },
                  { "status", channel.status },
                  { "priority", channel.priority } });
}

json channel_ref_json(const Channel &channel)
{
    return json({ { "id", channel.id }, { "name", channel.name }, { "type", channel.type } });
}

std::vector<std::string_view> split_path_parts(std::string_view path)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start < path.size()) {
        while (start < path.size() && path[start] == '/') {
            ++start;
        }
        if (start >= path.size()) {
            break;
        }
        size_t end = path.find('/', start);
        if (end == std::string_view::npos) {
            parts.push_back(path.substr(start));
            break;
        }
        parts.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

bool parse_positive_path_id(std::string_view raw, long long &out)
{
    const auto id = parse_positive_i64_or(raw);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

HttpResponse admin_channel_groups_list_response(std::string_view request_id)
{
    ChannelGroupStore &store = ChannelGroupStore::instance();
    const std::vector<ChannelGroup> groups = store.list_channel_groups();

    json data = json::array();
    for (const ChannelGroup &group : groups) {
        data.push_back(to_json(group));
    }
    return http_response(200, "OK", json({ { "success", true }, { "data", std::move(data) } }),
                         { { "X-Request-Id", std::string{ request_id } } });
}

HttpResponse admin_channel_groups_create_response(std::string_view body, std::string_view request_id)
{
    auto object = parse_json_object(body);
    if (!object.has_value()) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    const std::string name = json_object_string(*object, "name");
    const std::string description = json_object_string(*object, "description");
    const double price_multiplier = (*object)["price_multiplier"].as_double().value_or(1.0);
    const bool status = parse_bool_value(json_value_to_string((*object)["status"])).value_or(true);

    try {
        ChannelGroupStore &store = ChannelGroupStore::instance();
        const int id = store.create_channel_group(name, description, price_multiplier, status);
        if (id <= 0) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "创建渠道组失败" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        return http_response(200, "OK", json({ { "success", true }, { "data", json({ { "id", id } }) } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "创建渠道组失败" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse admin_channel_group_detail_response(std::string_view request_id, long long group_id)
{
    try {
        ChannelGroupStore &group_store = ChannelGroupStore::instance();
        ChannelStore &channel_store = ChannelStore::instance();
        const ChannelGroup group = group_store.get_channel_group_by_id(group_id);
        if (group.id <= 0) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "渠道组不存在" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        const auto channels = channel_store.list_channels();

        json members = json::array();
        for (const Channel &channel : group.channels) {
            members.push_back(channel_group_member_json(channel));
        }

        json channel_list = json::array();
        for (const Channel &channel : channels) {
            channel_list.push_back(channel_ref_json(channel));
        }

        return http_response(200, "OK",
                             json({ { "success", true },
                                    { "data", json({ { "group", to_json(group) },
                                                     { "members", std::move(members) },
                                                     { "channels", std::move(channel_list) } }) } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "加载渠道组详情失败" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse admin_channel_group_update_response(std::string_view body, std::string_view request_id, long long group_id)
{
    auto object = parse_json_object(body);
    if (!object.has_value()) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    try {
        ChannelGroupStore &store = ChannelGroupStore::instance();
        ChannelGroup group = store.get_channel_group_by_id(group_id);
        if (group.id <= 0) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "渠道组不存在" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }

        group.name = (*object)["name"].as_string().value_or(group.name);
        group.description = (*object)["description"].as_string().value_or(group.description);
        group.price_multiplier = (*object)["price_multiplier"].as_double().value_or(group.price_multiplier);

        if (!store.update_channel_group(group_id, group.name, group.description, group.price_multiplier)) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "渠道组不存在" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        return http_response(200, "OK", json({ { "success", true } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "更新渠道组失败" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse admin_channel_group_delete_response(std::string_view request_id, long long group_id)
{
    try {
        ChannelGroupStore &store = ChannelGroupStore::instance();
        if (!store.delete_channel_group(group_id)) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "渠道组不存在" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        return http_response(200, "OK", json({ { "success", true } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "删除渠道组失败" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse admin_channel_group_add_member_response(std::string_view body, std::string_view request_id,
                                                     long long group_id)
{
    auto object = parse_json_object(body);
    if (!object.has_value()) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
    const long long channel_id = (*object)["channel_id"].as_int64().value_or(0);
    try {
        ChannelGroupStore &store = ChannelGroupStore::instance();
        const auto channel = ChannelStore::instance().find_channel(channel_id);
        if (!channel.has_value() || !store.add_channel_group_member(group_id, *channel)) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "渠道组或渠道不存在" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        return http_response(200, "OK", json({ { "success", true } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "添加成员失败" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse admin_channel_group_delete_member_response(std::string_view request_id, long long group_id,
                                                        long long channel_id)
{
    try {
        ChannelGroupStore &store = ChannelGroupStore::instance();
        if (!store.remove_channel_group_member(group_id, channel_id)) {
            return http_response(200, "OK", json({ { "success", false }, { "message", "渠道组或成员不存在" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        }
        return http_response(200, "OK", json({ { "success", true } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return http_response(200, "OK", json({ { "success", false }, { "message", err.what() } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return http_response(200, "OK", json({ { "success", false }, { "message", "移除成员失败" } }),
                             { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse channel_groups_admin_dispatch(std::string_view raw_request, std::string_view body,
                                           const ChannelGroupsAdminParsedRequest &parsed_in,
                                           std::string_view request_id)
{
    ParsedRequest parsed{ parsed_in.method, parsed_in.path, parsed_in.target };
    RootAuth auth = authenticate_root_admin(raw_request);
    if (!auth.ok) {
        return admin_auth_failure(request_id, auth.failure, auth.clear_cookie, raw_request);
    }

    const auto parts = split_path_parts(parsed.path);
    if (parts.size() == 3 && parsed.method == "GET")
        return admin_channel_groups_list_response(request_id);
    if (parts.size() == 3 && parsed.method == "POST")
        return admin_channel_groups_create_response(body, request_id);
    if (parts.size() < 4)
        return http_response(404, "Not Found", json("not found"), { { "X-Request-Id", std::string{ request_id } } });

    long long group_id = 0;
    if (!parse_positive_path_id(parts[3], group_id))
        return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                             { { "X-Request-Id", std::string{ request_id } } });

    if (parts.size() == 5 && parts[4] == "detail" && parsed.method == "GET")
        return admin_channel_group_detail_response(request_id, group_id);
    if (parts.size() == 4 && parsed.method == "PUT")
        return admin_channel_group_update_response(body, request_id, group_id);
    if (parts.size() == 4 && parsed.method == "DELETE")
        return admin_channel_group_delete_response(request_id, group_id);
    if (parts.size() == 6 && parts[4] == "children" && parts[5] == "channels" && parsed.method == "POST")
        return admin_channel_group_add_member_response(body, request_id, group_id);
    if (parts.size() == 7 && parts[4] == "children" && parts[5] == "channels" && parsed.method == "DELETE") {
        long long channel_id = 0;
        if (!parse_positive_path_id(parts[6], channel_id))
            return http_response(200, "OK", json({ { "success", false }, { "message", "无效的参数" } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
        return admin_channel_group_delete_member_response(request_id, group_id, channel_id);
    }
    return http_response(404, "Not Found", json("not found"), { { "X-Request-Id", std::string{ request_id } } });
}

} // namespace

HttpResponse channel_groups_admin_route(std::string_view raw_request, std::string_view body,
                                        const ChannelGroupsAdminParsedRequest &parsed, std::string_view request_id)
{
    return channel_groups_admin_dispatch(raw_request, body, parsed, request_id);
}

} // namespace revlm
