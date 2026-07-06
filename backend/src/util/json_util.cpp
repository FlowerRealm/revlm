#include "util/json_util.hpp"

#include <cctype>
#include <limits>
#include <tuple>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

std::string json_string_to_std(boost::json::string_view value)
{
    return std::string{ value.data(), value.size() };
}

} // namespace

std::optional<boost::json::value> parse_json(std::string_view json)
{
    boost::system::error_code ec;
    boost::json::value value = boost::json::parse(boost::json::string_view{ json.data(), json.size() }, ec);
    if (ec) {
        return std::nullopt;
    }
    return value;
}

std::optional<boost::json::object> parse_json_object(std::string_view json)
{
    const auto value = parse_json(json);
    if (!value || !value->is_object()) {
        return std::nullopt;
    }
    return value->as_object();
}

std::optional<long long> json_int64(const boost::json::value &v)
{
    if (v.is_int64()) {
        return v.as_int64();
    }
    if (v.is_uint64()) {
        const uint64_t value = v.as_uint64();
        if (value > static_cast<uint64_t>(std::numeric_limits<long long>::max())) {
            return std::nullopt;
        }
        return static_cast<long long>(value);
    }
    return std::nullopt;
}

std::optional<std::string> find_json_string_field(const boost::json::value &v, std::string_view field_name)
{
    if (v.is_object()) {
        for (const auto &field : v.as_object()) {
            if (field.key() == field_name && field.value().is_string()) {
                return json_string_to_std(field.value().as_string());
            }
            if (const auto nested = find_json_string_field(field.value(), field_name)) {
                return nested;
            }
        }
        return std::nullopt;
    }
    if (v.is_array()) {
        for (const auto &child : v.as_array()) {
            if (const auto nested = find_json_string_field(child, field_name)) {
                return nested;
            }
        }
    }
    return std::nullopt;
}

std::optional<long long> find_json_int_field(const boost::json::value &v, std::string_view field_name)
{
    if (v.is_object()) {
        for (const auto &field : v.as_object()) {
            if (field.key() == field_name) {
                if (const auto value = json_int64(field.value())) {
                    return value;
                }
            }
            if (const auto nested = find_json_int_field(field.value(), field_name)) {
                return nested;
            }
        }
        return std::nullopt;
    }
    if (v.is_array()) {
        for (const auto &child : v.as_array()) {
            if (const auto nested = find_json_int_field(child, field_name)) {
                return nested;
            }
        }
    }
    return std::nullopt;
}

std::optional<bool> parse_json_bool_field(std::string_view json, std::string_view field_name)
{
    const auto doc = parse_json(json);
    if (!doc || !doc->is_object()) {
        return std::nullopt;
    }
    for (const auto &field : doc->as_object()) {
        if (field.key() != field_name) {
            continue;
        }
        if (field.value().is_bool()) {
            return field.value().as_bool();
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> parse_json_string_field(std::string_view json, std::string_view field_name)
{
    const auto doc = parse_json(json);
    if (!doc) {
        return std::nullopt;
    }
    return find_json_string_field(*doc, field_name);
}

std::optional<long long> parse_json_int_field(std::string_view json, std::string_view field_name)
{
    const auto doc = parse_json(json);
    if (!doc) {
        return std::nullopt;
    }
    return find_json_int_field(*doc, field_name);
}

bool parse_json_object_string_fields(std::string_view json, std::vector<std::pair<std::string, std::string>> &fields)
{
    fields.clear();
    const auto doc = parse_json_object(json);
    if (!doc) {
        return false;
    }
    for (const auto &field : *doc) {
        if (!field.value().is_string()) {
            continue;
        }
        fields.emplace_back(json_string_to_std(field.key()), json_string_to_std(field.value().as_string()));
    }
    return true;
}

std::string json_value_to_string(const boost::json::value &v)
{
    if (v.is_string()) {
        return json_string_to_std(v.as_string());
    }
    if (v.is_null()) {
        return {};
    }
    return boost::json::serialize(v);
}

bool parse_json_object_mixed_fields(std::string_view json,
                                    std::vector<std::tuple<std::string, std::string, bool>> &fields)
{
    fields.clear();
    const auto doc = parse_json_object(json);
    if (!doc) {
        return false;
    }
    for (const auto &field : *doc) {
        if (field.value().is_string()) {
            fields.emplace_back(json_string_to_std(field.key()), json_string_to_std(field.value().as_string()), true);
            continue;
        }
        fields.emplace_back(json_string_to_std(field.key()), json_value_to_string(field.value()), false);
    }
    return true;
}

std::optional<std::string> extract_json_object_field(std::string_view json, std::string_view field_name)
{
    const auto doc = parse_json_object(json);
    if (!doc) {
        return std::nullopt;
    }
    const boost::json::value *field = doc->if_contains(field_name);
    if (field == nullptr || !field->is_object()) {
        return std::nullopt;
    }
    return boost::json::serialize(*field);
}

bool parse_json_string_array(std::string_view json, std::vector<std::string> &out)
{
    out.clear();
    const auto doc = parse_json(json);
    if (!doc || !doc->is_array()) {
        return false;
    }
    for (const auto &item : doc->as_array()) {
        if (!item.is_string()) {
            return false;
        }
        out.emplace_back(json_string_to_std(item.as_string()));
    }
    return true;
}

bool parse_json_long_long(std::string_view raw, long long &out)
{
    const std::string value = trim_ascii(raw);
    if (value.empty()) {
        return false;
    }
    size_t pos = 0;
    try {
        const long long parsed = std::stoll(value, &pos, 10);
        if (pos != value.size()) {
            return false;
        }
        out = parsed;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool parse_json_long_long(const boost::json::value &value, long long &out)
{
    if (value.is_int64()) {
        out = value.as_int64();
        return true;
    }
    if (value.is_uint64() &&
        value.as_uint64() <= static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
        out = static_cast<long long>(value.as_uint64());
        return true;
    }
    if (value.is_string()) {
        return parse_json_long_long(json_string_to_std(value.as_string()), out);
    }
    return false;
}

bool parse_json_int(const boost::json::value &value, int &out)
{
    long long parsed = 0;
    if (!parse_json_long_long(value, parsed) || parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

bool parse_json_bool(const boost::json::value &value, bool &out)
{
    if (!value.is_bool()) {
        return false;
    }
    out = value.as_bool();
    return true;
}

bool parse_json_number_array(std::string_view body, std::vector<long long> &out)
{
    out.clear();
    const auto doc = parse_json(body);
    if (!doc || !doc->is_array()) {
        return false;
    }
    for (const auto &item : doc->as_array()) {
        long long value = 0;
        if (!parse_json_long_long(item, value)) {
            return false;
        }
        out.push_back(value);
    }
    return true;
}

} // namespace revlm
