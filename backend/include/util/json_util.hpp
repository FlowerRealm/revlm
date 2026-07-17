#pragma once

#include "util/json.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace revlm
{

std::optional<json> parse_json_object(std::string_view text);

std::optional<long long> json_int64(const json &v);
std::optional<long long> find_json_int_field(const json &v, std::string_view field);
std::optional<std::string> find_json_string_field(const json &v, std::string_view field);

std::optional<bool> parse_json_bool_field(std::string_view text, std::string_view field);
std::optional<std::string> parse_json_string_field(std::string_view text, std::string_view field);
std::optional<long long> parse_json_int_field(std::string_view text, std::string_view field);

/** Top-level string field; empty if missing or not a string. */
std::string json_object_string(const json &object, std::string_view key);

bool parse_json_string_array(std::string_view text, std::vector<std::string> &out);
std::optional<std::string> extract_json_object_field(std::string_view text, std::string_view field);

std::string json_value_to_string(const json &v);

bool parse_json_long_long(std::string_view raw, long long &out);
bool parse_json_long_long(const json &value, long long &out);
bool parse_json_int(const json &value, int &out);
bool parse_json_bool(const json &value, bool &out);
bool parse_json_number_array(std::string_view body, std::vector<long long> &out);

} // namespace revlm
