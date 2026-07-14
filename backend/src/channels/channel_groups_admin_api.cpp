#include "channels/channel_groups.hpp"

#include "auth/session.hpp"
#include "auth/users.hpp"
#include "channels/channels.hpp"
#include "runtime/runtime_workers.hpp"
#include "store/database.hpp"
#include "util/json_util.hpp"
#include "util/user_input.hpp"

#include <boost/json.hpp>

#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

struct JsonField {
    std::string name;
    std::string value;
};

struct JsonValueField {
    std::string name;
    const boost::json::value *value = nullptr;
};

struct RootAuth {
    bool ok = false;
    bool clear_cookie = false;
    std::string failure;
};

HttpResponse api_json_response(std::string body, std::string_view request_id, const std::vector<Header> &headers = {})
{
    return http_response(200, "OK", body, "application/json; charset=utf-8", request_id, headers);
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

std::string api_success(std::string_view data_json = {})
{
    std::string body = "{\"success\":true,\"message\":\"\"";
    if (!data_json.empty()) {
        body += ",\"data\":";
        body += data_json;
    }
    body += "}\n";
    return body;
}

std::string api_failure(std::string_view message)
{
    return "{\"success\":false,\"message\":\"" + json_escape(message) + "\"}\n";
}

RootAuth authenticate_root_admin(std::string_view raw_request, const Config &config)
{
    RootAuth out;
    const WebSessionAuth auth = authenticate_root_web_session(raw_request, config);
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
    std::vector<Header> headers;
    if (clear_cookie) {
        headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
    }
    return api_json_response(api_failure(message), request_id, headers);
}

std::string channel_group_json(const ChannelGroup &group)
{
    char price_buf[32];
    std::snprintf(price_buf, sizeof(price_buf), "%.6f", group.price_multiplier);
    return "{\"id\":" + std::to_string(group.id) + ",\"name\":\"" + json_escape(group.name) + "\"" +
           ",\"description\":\"" + json_escape(group.description) + "\"" + ",\"price_multiplier\":" + price_buf +
           ",\"status\":" + std::to_string(group.status) + "}";
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

std::string json_string_to_std(boost::json::string_view value)
{
    return std::string{ value.data(), value.size() };
}

bool parse_json_object_strings(const boost::json::object &object, std::vector<JsonField> &fields)
{
    fields.clear();
    for (const auto &field : object) {
        if (field.value().is_string()) {
            fields.push_back(
                JsonField{ json_string_to_std(field.key()), json_string_to_std(field.value().as_string()) });
        }
    }
    return true;
}

bool parse_json_object_values(const boost::json::object &object, std::vector<JsonValueField> &fields)
{
    fields.clear();
    for (const auto &field : object) {
        fields.push_back(JsonValueField{ json_string_to_std(field.key()), &field.value() });
    }
    return true;
}

std::string json_field(const std::vector<JsonField> &fields, std::string_view name)
{
    for (const JsonField &field : fields) {
        if (field.name == name) {
            return field.value;
        }
    }
    return {};
}

std::optional<JsonValueField> json_value_field(const std::vector<JsonValueField> &fields, std::string_view name)
{
    for (const JsonValueField &field : fields) {
        if (field.name == name) {
            return field;
        }
    }
    return std::nullopt;
}

bool json_value_is_null(const JsonValueField &field)
{
    return field.value != nullptr && field.value->is_null();
}

void notify_routing_change(std::string_view, long long)
{
    notify_runtime_routing_invalidated();
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

HttpResponse admin_channel_groups_list_response(const Config &config, std::string_view request_id)
{
    auto db = make_database(config.db_dsn);
    ChannelGroupStore store(*db);
    const std::vector<ChannelGroup> groups = store.list_channel_groups();

    std::string data = "[";
    for (size_t i = 0; i < groups.size(); ++i) {
        if (i > 0) {
            data += ",";
        }
        data += channel_group_json(groups[i]);
    }
    data += "]";
    return api_json_response(api_success(data), request_id);
}

HttpResponse admin_channel_groups_create_response(std::string_view body, const Config &config,
                                                  std::string_view request_id)
{
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    std::vector<JsonValueField> fields;
    (void)parse_json_object_values(*object, fields);
    const auto name_field = json_value_field(fields, "name");
    const auto desc_field = json_value_field(fields, "description");
    const auto price_field = json_value_field(fields, "price_multiplier");
    const auto status_field = json_value_field(fields, "status");
    if (!name_field.has_value() || !name_field->value->is_string()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    std::vector<JsonField> string_fields;
    (void)parse_json_object_strings(*object, string_fields);
    const std::string name = json_field(string_fields, "name");
    std::string description;
    if (desc_field.has_value()) {
        if (!json_value_is_null(*desc_field) && !desc_field->value->is_string()) {
            return api_json_response(api_failure("无效的参数"), request_id);
        }
        if (!json_value_is_null(*desc_field)) {
            description = json_field(string_fields, "description");
        }
    }
    double price_multiplier = 1.0;
    if (price_field.has_value()) {
        if (!price_field->value->is_double() && !price_field->value->is_int64() && !price_field->value->is_uint64()) {
            return api_json_response(api_failure("无效的参数"), request_id);
        }
        price_multiplier = price_field->value->to_number<double>();
        if (!(price_multiplier > 0.0)) {
            return api_json_response(api_failure("无效的参数"), request_id);
        }
    }
    int status = 1;
    if (status_field.has_value()) {
        if (!status_field->value->is_int64() && !status_field->value->is_uint64()) {
            return api_json_response(api_failure("无效的参数"), request_id);
        }
        status = static_cast<int>(status_field->value->to_number<int64_t>());
    }

    try {
        auto db = make_database(config.db_dsn);
        ChannelGroupStore store(*db);
        const int id = store.create_channel_group(name, description, price_multiplier, status);
        if (id <= 0) {
            return api_json_response(api_failure("创建渠道组失败"), request_id);
        }
        notify_routing_change("create", id);
        return api_json_response(api_success("{\"id\":" + std::to_string(id) + "}"), request_id);
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("创建渠道组失败"), request_id);
    }
}

HttpResponse admin_channel_group_detail_response(const Config &config, std::string_view request_id, long long group_id)
{
    try {
        auto db = make_database(config.db_dsn);
        ChannelGroupStore group_store(*db);
        ChannelStore channel_store(*db);
        const ChannelGroup group = group_store.get_channel_group_by_id(group_id);
        if (group.id <= 0) {
            return api_json_response(api_failure("渠道组不存在"), request_id);
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
        return api_json_response(api_success(body), request_id);
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("加载渠道组详情失败"), request_id);
    }
}

HttpResponse admin_channel_group_update_response(std::string_view body, const Config &config,
                                                 std::string_view request_id, long long group_id)
{
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    std::vector<JsonValueField> value_fields;
    std::vector<JsonField> string_fields;
    (void)parse_json_object_values(*object, value_fields);
    (void)parse_json_object_strings(*object, string_fields);
    const auto name_field = json_value_field(value_fields, "name");
    const auto desc_field = json_value_field(value_fields, "description");
    const auto price_field = json_value_field(value_fields, "price_multiplier");

    try {
        auto db = make_database(config.db_dsn);
        ChannelGroupStore store(*db);
        ChannelGroup group = store.get_channel_group_by_id(group_id);
        if (group.id <= 0) {
            return api_json_response(api_failure("渠道组不存在"), request_id);
        }

        if (name_field.has_value() && !json_value_is_null(*name_field)) {
            if (!name_field->value->is_string()) {
                return api_json_response(api_failure("无效的参数"), request_id);
            }
            group.name = json_field(string_fields, "name");
        }
        if (desc_field.has_value() && !json_value_is_null(*desc_field)) {
            if (!desc_field->value->is_string()) {
                return api_json_response(api_failure("无效的参数"), request_id);
            }
            group.description = json_field(string_fields, "description");
        }
        if (price_field.has_value()) {
            if (!price_field->value->is_double() && !price_field->value->is_int64() &&
                !price_field->value->is_uint64()) {
                return api_json_response(api_failure("无效的参数"), request_id);
            }
            const double price_multiplier = price_field->value->to_number<double>();
            if (!(price_multiplier > 0.0)) {
                return api_json_response(api_failure("无效的参数"), request_id);
            }
            group.price_multiplier = price_multiplier;
        }

        if (!store.update_channel_group(group_id, group.name, group.description, group.price_multiplier)) {
            return api_json_response(api_failure("渠道组不存在"), request_id);
        }
        notify_routing_change("update", group_id);
        return api_json_response(api_success(), request_id);
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("更新渠道组失败"), request_id);
    }
}

HttpResponse admin_channel_group_delete_response(const Config &config, std::string_view request_id, long long group_id)
{
    try {
        auto db = make_database(config.db_dsn);
        ChannelGroupStore store(*db);
        if (!store.delete_channel_group(group_id)) {
            return api_json_response(api_failure("渠道组不存在"), request_id);
        }
        notify_routing_change("delete", group_id);
        return api_json_response(api_success(), request_id);
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("删除渠道组失败"), request_id);
    }
}

HttpResponse admin_channel_group_add_member_response(std::string_view body, const Config &config,
                                                     std::string_view request_id, long long group_id)
{
    const auto object = parse_json_object(body);
    if (!object.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    std::vector<JsonValueField> fields;
    (void)parse_json_object_values(*object, fields);
    const auto channel_field = json_value_field(fields, "channel_id");
    if (!channel_field.has_value()) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    long long channel_id = 0;
    if (!parse_json_long_long(*channel_field->value, channel_id) || channel_id <= 0) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        ChannelGroupStore store(*db);
        ChannelStore channel_store(*db);
        std::optional<Channel> channel;
        for (const Channel &candidate : channel_store.list_channels()) {
            if (candidate.id == channel_id) {
                channel = candidate;
                break;
            }
        }
        if (!channel.has_value() || !store.add_channel_group_member(group_id, *channel)) {
            return api_json_response(api_failure("渠道组或渠道不存在"), request_id);
        }
        notify_routing_change("member_add", group_id);
        return api_json_response(api_success(), request_id);
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("添加成员失败"), request_id);
    }
}

HttpResponse admin_channel_group_delete_member_response(const Config &config, std::string_view request_id,
                                                        long long group_id, long long channel_id)
{
    try {
        auto db = make_database(config.db_dsn);
        ChannelGroupStore store(*db);
        if (!store.remove_channel_group_member(group_id, channel_id)) {
            return api_json_response(api_failure("渠道组或成员不存在"), request_id);
        }
        notify_routing_change("member_delete", group_id);
        return api_json_response(api_success(), request_id);
    } catch (const std::invalid_argument &err) {
        return api_json_response(api_failure(err.what()), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("移除成员失败"), request_id);
    }
}

HttpResponse channel_groups_admin_dispatch(std::string_view raw_request, std::string_view body,
                                           const ChannelGroupsAdminParsedRequest &parsed_in, const Config &config,
                                           std::string_view request_id)
{
    ParsedRequest parsed{ parsed_in.method, parsed_in.path, parsed_in.target };
    RootAuth auth = authenticate_root_admin(raw_request, config);
    if (!auth.ok) {
        return admin_auth_failure(request_id, auth.failure, auth.clear_cookie, raw_request);
    }

    const auto parts = split_path_parts(parsed.path);
    if (parts.size() == 3 && parsed.method == "GET") {
        return admin_channel_groups_list_response(config, request_id);
    }
    if (parts.size() == 3 && parsed.method == "POST") {
        return admin_channel_groups_create_response(body, config, request_id);
    }
    if (parts.size() < 4) {
        return http_response(404, "Not Found", "not found\n", "text/plain; charset=utf-8", request_id);
    }

    long long group_id = 0;
    if (!parse_positive_path_id(parts[3], group_id)) {
        return api_json_response(api_failure("无效的参数"), request_id);
    }

    if (parts.size() == 5 && parts[4] == "detail" && parsed.method == "GET") {
        return admin_channel_group_detail_response(config, request_id, group_id);
    }
    if (parts.size() == 4 && parsed.method == "PUT") {
        return admin_channel_group_update_response(body, config, request_id, group_id);
    }
    if (parts.size() == 4 && parsed.method == "DELETE") {
        return admin_channel_group_delete_response(config, request_id, group_id);
    }
    if (parts.size() == 6 && parts[4] == "children" && parts[5] == "channels" && parsed.method == "POST") {
        return admin_channel_group_add_member_response(body, config, request_id, group_id);
    }
    if (parts.size() == 7 && parts[4] == "children" && parts[5] == "channels" && parsed.method == "DELETE") {
        long long channel_id = 0;
        if (!parse_positive_path_id(parts[6], channel_id)) {
            return api_json_response(api_failure("无效的参数"), request_id);
        }
        return admin_channel_group_delete_member_response(config, request_id, group_id, channel_id);
    }
    return http_response(404, "Not Found", "not found\n", "text/plain; charset=utf-8", request_id);
}

} // namespace

HttpResponse channel_groups_admin_route(std::string_view raw_request, std::string_view body,
                                        const ChannelGroupsAdminParsedRequest &parsed, const Config &config,
                                        std::string_view request_id)
{
    return channel_groups_admin_dispatch(raw_request, body, parsed, config, request_id);
}

} // namespace revlm
