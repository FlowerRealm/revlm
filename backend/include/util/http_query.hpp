#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace revlm
{

struct QueryParam {
    std::string key;
    std::string value;
};

std::map<std::string, std::string> parse_query_map(std::string_view target);
std::vector<QueryParam> parse_query_params(std::string_view target);

std::string query_param_value(const std::map<std::string, std::string> &params, std::string_view key);
std::string query_param_value(const std::vector<QueryParam> &params, std::string_view key);

} // namespace revlm
