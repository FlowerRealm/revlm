#include "util/json_util.hpp"

#include "util/strings.hpp"

#include <limits>

namespace revlm
{

std::optional<json> parse_json_object(std::string_view text)
{
    auto value = json::parse(text);
    if (!value || !value->is_object()) {
        return std::nullopt;
    }
    return value;
}

std::optional<long long> json_int64(const json &v)
{
    return v.as_int64();
}

std::optional<std::string> find_json_string_field(const json &v, std::string_view field_name)
{
    if (v.is_object()) {
        for (const auto &key : v.keys()) {
            const json child = v[key];
            if (key == field_name && child.is_string()) {
                return child.as_string();
            }
            if (const auto nested = find_json_string_field(child, field_name)) {
                return nested;
            }
        }
        return std::nullopt;
    }
    if (v.is_array()) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (const auto nested = find_json_string_field(v[i], field_name)) {
                return nested;
            }
        }
    }
    return std::nullopt;
}

std::optional<long long> find_json_int_field(const json &v, std::string_view field_name)
{
    if (v.is_object()) {
        for (const auto &key : v.keys()) {
            const json child = v[key];
            if (key == field_name) {
                if (const auto value = json_int64(child)) {
                    return value;
                }
            }
            if (const auto nested = find_json_int_field(child, field_name)) {
                return nested;
            }
        }
        return std::nullopt;
    }
    if (v.is_array()) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (const auto nested = find_json_int_field(v[i], field_name)) {
                return nested;
            }
        }
    }
    return std::nullopt;
}

std::optional<bool> parse_json_bool_field(std::string_view text, std::string_view field_name)
{
    const auto doc = parse_json_object(text);
    if (!doc) {
        return std::nullopt;
    }
    if (!doc->contains(field_name)) {
        return std::nullopt;
    }
    return (*doc)[field_name].as_bool();
}

std::optional<std::string> parse_json_string_field(std::string_view text, std::string_view field_name)
{
    const auto doc = json::parse(text);
    if (!doc) {
        return std::nullopt;
    }
    return find_json_string_field(*doc, field_name);
}

std::optional<long long> parse_json_int_field(std::string_view text, std::string_view field_name)
{
    const auto doc = json::parse(text);
    if (!doc) {
        return std::nullopt;
    }
    return find_json_int_field(*doc, field_name);
}

std::string json_object_string(const json &object, std::string_view key)
{
    if (!object.is_object() || !object.contains(key)) {
        return {};
    }
    const json value = object[key];
    if (!value.is_string()) {
        return {};
    }
    return *value.as_string();
}

std::string json_value_to_string(const json &v)
{
    if (v.is_string()) {
        return *v.as_string();
    }
    if (v.is_null()) {
        return {};
    }
    return v.dump();
}

std::optional<std::string> extract_json_object_field(std::string_view text, std::string_view field_name)
{
    const auto doc = parse_json_object(text);
    if (!doc || !doc->contains(field_name)) {
        return std::nullopt;
    }
    const json field = (*doc)[field_name];
    if (!field.is_object()) {
        return std::nullopt;
    }
    return field.dump();
}

bool parse_json_string_array(std::string_view text, std::vector<std::string> &out)
{
    out.clear();
    const auto doc = json::parse(text);
    if (!doc || !doc->is_array()) {
        return false;
    }
    for (std::size_t i = 0; i < doc->size(); ++i) {
        const json item = (*doc)[i];
        if (!item.is_string()) {
            return false;
        }
        out.push_back(*item.as_string());
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

bool parse_json_long_long(const json &value, long long &out)
{
    if (const auto n = value.as_int64()) {
        out = *n;
        return true;
    }
    if (value.is_string()) {
        return parse_json_long_long(std::string_view{ *value.as_string() }, out);
    }
    return false;
}

bool parse_json_int(const json &value, int &out)
{
    long long parsed = 0;
    if (!parse_json_long_long(value, parsed) || parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

bool parse_json_bool(const json &value, bool &out)
{
    if (const auto b = value.as_bool()) {
        out = *b;
        return true;
    }
    return false;
}

bool parse_json_number_array(std::string_view body, std::vector<long long> &out)
{
    out.clear();
    const auto doc = json::parse(body);
    if (!doc || !doc->is_array()) {
        return false;
    }
    for (std::size_t i = 0; i < doc->size(); ++i) {
        long long value = 0;
        if (!parse_json_long_long((*doc)[i], value)) {
            return false;
        }
        out.push_back(value);
    }
    return true;
}

} // namespace revlm
