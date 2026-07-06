#pragma once

#include <boost/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace revlm
{

std::optional<boost::json::value> parse_json(std::string_view json);
std::optional<boost::json::object> parse_json_object(std::string_view json);

std::optional<long long> json_int64(const boost::json::value &v);
std::optional<long long> find_json_int_field(const boost::json::value &v, std::string_view field);
std::optional<std::string> find_json_string_field(const boost::json::value &v, std::string_view field);

std::optional<bool> parse_json_bool_field(std::string_view json, std::string_view field);
std::optional<std::string> parse_json_string_field(std::string_view json, std::string_view field);
std::optional<long long> parse_json_int_field(std::string_view json, std::string_view field);

bool parse_json_object_string_fields(std::string_view json, std::vector<std::pair<std::string, std::string>> &fields);
bool parse_json_object_mixed_fields(std::string_view json,
                                    std::vector<std::tuple<std::string, std::string, bool>> &fields);
bool parse_json_string_array(std::string_view json, std::vector<std::string> &out);
std::optional<std::string> extract_json_object_field(std::string_view json, std::string_view field);

std::string json_value_to_string(const boost::json::value &v);

bool parse_json_long_long(std::string_view raw, long long &out);
bool parse_json_long_long(const boost::json::value &value, long long &out);
bool parse_json_int(const boost::json::value &value, int &out);
bool parse_json_bool(const boost::json::value &value, bool &out);
bool parse_json_number_array(std::string_view body, std::vector<long long> &out);

} // namespace revlm
