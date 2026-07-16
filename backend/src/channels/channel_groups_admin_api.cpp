#include "channels/channel_groups.hpp"

#include "auth/session.hpp"
#include "auth/users.hpp"
#include "channels/channels.hpp"
#include "store/database.hpp"
#include "util/json_convert.hpp"
#include "util/json_util.hpp"
#include "util/user_input.hpp"

#include <boost/json.hpp>

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

HttpResponse api_json_response(boost::json::value body, std::vector<Header> headers = {})
{
    return http_response(200, "OK", std::move(body), std::move(headers));
}

boost::json::value api_success()
{
    boost::json::object body;
    body["success"] = true;
    body["message"] = "";
    return body;
}

boost::json::value api_success(boost::json::value data)
{
    boost::json::object body;
    body["success"] = true;
    body["message"] = "";
    body["data"] = std::move(data);
    return body;
}

boost::json::value api_failure(std::string_view message)
{
    boost::json::object body;
    body["success"] = false;
    body["message"] = message;
    return body;
}

std::string json_escape(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    return out;
}

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
    return api_json_response(api_failure(message), std::move(headers));
}

std::string channel_group_json(const ChannelGroup &group)
{
    return boost::json::serialize(to_json(group));
}

std::string channel_group_member_json(const Channel &channel)
{
    return "{\"channel_id\":" + std::to_string(channel.id) + ",\"name\":\"" + json_escape(channel.name) + "\"" +
           ",\"type\":" + std::to_string(channel.type) +
           ",\"status\":" + std::string(channel.status ? "true" : "false") +
           ",\"priority\":" + std::to_string(channel.priority) + "}";
}

std::string channel_ref_json(const Channel &channel)
{
    return "{\"id\":" + std::to_string(channel.id) + ",\"name\":\"" + json_escape(channel.name) + "\"" +
           ",\"type\":" + std::to_string(channel.type) + "}";
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

    boost::json::array data;
    for (const ChannelGroup &group : groups) {
        data.push_back(to_json(group));
    }
    return api_json_response(api_success(std::move(data)), { { "X-Request-Id", std::string{ request_id } } });
}

HttpResponse admin_channel_groups_create_response(std::string_view body, std::string_view request_id)
{
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
    }
    const boost::json::value *name_field = object->if_contains("name");
    const boost::json::value *desc_field = object->if_contains("description");
    const boost::json::value *price_field = object->if_contains("price_multiplier");
    const boost::json::value *status_field = object->if_contains("status");
    if (name_field == nullptr || !name_field->is_string()) {
        return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
    }

    const std::string name = json_object_string(*object, "name");
    std::string description;
    if (desc_field != nullptr) {
        if (!desc_field->is_null() && !desc_field->is_string()) {
            return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
        }
        if (!desc_field->is_null()) {
            description = json_object_string(*object, "description");
        }
    }
    double price_multiplier = 1.0;
    if (price_field != nullptr) {
        if (!price_field->is_double() && !price_field->is_int64() && !price_field->is_uint64()) {
            return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
        }
        price_multiplier = price_field->to_number<double>();
        if (!(price_multiplier > 0.0)) {
            return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
        }
    }
    int status = 1;
    if (status_field != nullptr) {
        if (!status_field->is_int64() && !status_field->is_uint64()) {
            return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
        }
        status = static_cast<int>(status_field->to_number<int64_t>());
    }

    try {
        ChannelGroupStore &store = ChannelGroupStore::instance();
        const int id = store.create_channel_group(name, description, price_multiplier, status);
        if (id <= 0) {
            return api_json_response(api_failure("创建渠道组失败"), { { "X-Request-Id", std::string{ request_id } } });
        }
        return api_json_response(api_success(boost::json::object{ { "id", id } }),
                                 { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return api_json_response(api_failure("创建渠道组失败"), { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse admin_channel_group_detail_response(std::string_view request_id, long long group_id)
{
    try {
        ChannelGroupStore &group_store = ChannelGroupStore::instance();
        ChannelStore &channel_store = ChannelStore::instance();
        const ChannelGroup group = group_store.get_channel_group_by_id(group_id);
        if (group.id <= 0) {
            return api_json_response(api_failure("渠道组不存在"), { { "X-Request-Id", std::string{ request_id } } });
        }
        const auto channels = channel_store.list_channels();

        std::string members_json = "[";
        for (size_t i = 0; i < group.channels.size(); ++i) {
            if (i > 0) {
                members_json += ",";
            }
            members_json += channel_group_member_json(group.channels[i]);
        }
        members_json += "]";

        std::string channels_json = "[";
        for (size_t i = 0; i < channels.size(); ++i) {
            if (i > 0) {
                channels_json += ",";
            }
            channels_json += channel_ref_json(channels[i]);
        }
        channels_json += "]";

        const std::string body = "{\"group\":" + channel_group_json(group) + ",\"members\":" + members_json +
                                 ",\"channels\":" + channels_json + "}";
        return api_json_response(api_success(*parse_json(body)), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return api_json_response(api_failure("加载渠道组详情失败"), { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse admin_channel_group_update_response(std::string_view body, std::string_view request_id, long long group_id)
{
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
    }
    const boost::json::value *name_field = object->if_contains("name");
    const boost::json::value *desc_field = object->if_contains("description");
    const boost::json::value *price_field = object->if_contains("price_multiplier");

    try {
        ChannelGroupStore &store = ChannelGroupStore::instance();
        ChannelGroup group = store.get_channel_group_by_id(group_id);
        if (group.id <= 0) {
            return api_json_response(api_failure("渠道组不存在"), { { "X-Request-Id", std::string{ request_id } } });
        }

        if (name_field != nullptr && !name_field->is_null()) {
            if (!name_field->is_string()) {
                return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
            }
            group.name = json_object_string(*object, "name");
        }
        if (desc_field != nullptr && !desc_field->is_null()) {
            if (!desc_field->is_string()) {
                return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
            }
            group.description = json_object_string(*object, "description");
        }
        if (price_field != nullptr) {
            if (!price_field->is_double() && !price_field->is_int64() && !price_field->is_uint64()) {
                return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
            }
            const double price_multiplier = price_field->to_number<double>();
            if (!(price_multiplier > 0.0)) {
                return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
            }
            group.price_multiplier = price_multiplier;
        }

        if (!store.update_channel_group(group_id, group.name, group.description, group.price_multiplier)) {
            return api_json_response(api_failure("渠道组不存在"), { { "X-Request-Id", std::string{ request_id } } });
        }
        return api_json_response(api_success(), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return api_json_response(api_failure("更新渠道组失败"), { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse admin_channel_group_delete_response(std::string_view request_id, long long group_id)
{
    try {
        ChannelGroupStore &store = ChannelGroupStore::instance();
        if (!store.delete_channel_group(group_id)) {
            return api_json_response(api_failure("渠道组不存在"), { { "X-Request-Id", std::string{ request_id } } });
        }
        return api_json_response(api_success(), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return api_json_response(api_failure("删除渠道组失败"), { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse admin_channel_group_add_member_response(std::string_view body, std::string_view request_id,
                                                     long long group_id)
{
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
    }
    const boost::json::value *channel_field = object->if_contains("channel_id");
    if (channel_field == nullptr) {
        return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
    }
    long long channel_id = 0;
    if (!parse_json_long_long(*channel_field, channel_id) || channel_id <= 0) {
        return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
    }
    try {
        ChannelGroupStore &store = ChannelGroupStore::instance();
        ChannelStore &channel_store = ChannelStore::instance();
        std::optional<Channel> channel;
        for (const Channel &candidate : channel_store.list_channels()) {
            if (candidate.id == channel_id) {
                channel = candidate;
                break;
            }
        }
        if (!channel.has_value() || !store.add_channel_group_member(group_id, *channel)) {
            return api_json_response(api_failure("渠道组或渠道不存在"),
                                     { { "X-Request-Id", std::string{ request_id } } });
        }
        return api_json_response(api_success(), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return api_json_response(api_failure("添加成员失败"), { { "X-Request-Id", std::string{ request_id } } });
    }
}

HttpResponse admin_channel_group_delete_member_response(std::string_view request_id, long long group_id,
                                                        long long channel_id)
{
    try {
        ChannelGroupStore &store = ChannelGroupStore::instance();
        if (!store.remove_channel_group_member(group_id, channel_id)) {
            return api_json_response(api_failure("渠道组或成员不存在"),
                                     { { "X-Request-Id", std::string{ request_id } } });
        }
        return api_json_response(api_success(), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), { { "X-Request-Id", std::string{ request_id } } });
    } catch (const std::exception &) {
        return api_json_response(api_failure("移除成员失败"), { { "X-Request-Id", std::string{ request_id } } });
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
    if (parts.size() == 3 && parsed.method == "GET") {
        return admin_channel_groups_list_response(request_id);
    }
    if (parts.size() == 3 && parsed.method == "POST") {
        return admin_channel_groups_create_response(body, request_id);
    }
    if (parts.size() < 4) {
        return http_response(404, "Not Found", boost::json::value("not found"),
                             { { "X-Request-Id", std::string{ request_id } } });
    }

    long long group_id = 0;
    if (!parse_positive_path_id(parts[3], group_id)) {
        return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
    }

    if (parts.size() == 5 && parts[4] == "detail" && parsed.method == "GET") {
        return admin_channel_group_detail_response(request_id, group_id);
    }
    if (parts.size() == 4 && parsed.method == "PUT") {
        return admin_channel_group_update_response(body, request_id, group_id);
    }
    if (parts.size() == 4 && parsed.method == "DELETE") {
        return admin_channel_group_delete_response(request_id, group_id);
    }
    if (parts.size() == 6 && parts[4] == "children" && parts[5] == "channels" && parsed.method == "POST") {
        return admin_channel_group_add_member_response(body, request_id, group_id);
    }
    if (parts.size() == 7 && parts[4] == "children" && parts[5] == "channels" && parsed.method == "DELETE") {
        long long channel_id = 0;
        if (!parse_positive_path_id(parts[6], channel_id)) {
            return api_json_response(api_failure("无效的参数"), { { "X-Request-Id", std::string{ request_id } } });
        }
        return admin_channel_group_delete_member_response(request_id, group_id, channel_id);
    }
    return http_response(404, "Not Found", boost::json::value("not found"),
                         { { "X-Request-Id", std::string{ request_id } } });
}

} // namespace

HttpResponse channel_groups_admin_route(std::string_view raw_request, std::string_view body,
                                        const ChannelGroupsAdminParsedRequest &parsed, std::string_view request_id)
{
    return channel_groups_admin_dispatch(raw_request, body, parsed, request_id);
}

} // namespace revlm
