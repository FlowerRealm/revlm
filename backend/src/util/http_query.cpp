#include "util/http_query.hpp"

#include <boost/url.hpp>

namespace revlm
{
namespace
{

boost::system::result<boost::urls::url_view> parse_target(std::string_view target)
{
    return boost::urls::parse_origin_form(target);
}

std::string decode_query_component(boost::urls::pct_string_view value)
{
    return value.decode();
}

} // namespace

std::map<std::string, std::string> parse_query_map(std::string_view target)
{
    std::map<std::string, std::string> out;
    const auto parsed = parse_target(target);
    if (!parsed) {
        return out;
    }
    for (const boost::urls::param_pct_view param : parsed->encoded_params()) {
        out[decode_query_component(param.key)] = param.has_value ? decode_query_component(param.value) : std::string{};
    }
    return out;
}

std::vector<QueryParam> parse_query_params(std::string_view target)
{
    std::vector<QueryParam> out;
    const auto parsed = parse_target(target);
    if (!parsed) {
        return out;
    }
    for (const boost::urls::param_pct_view param : parsed->encoded_params()) {
        out.push_back(QueryParam{
            .key = decode_query_component(param.key),
            .value = param.has_value ? decode_query_component(param.value) : std::string{},
        });
    }
    return out;
}

std::string query_param_value(const std::map<std::string, std::string> &params, std::string_view key)
{
    const auto it = params.find(std::string{ key });
    return it == params.end() ? std::string{} : it->second;
}

std::string query_param_value(const std::vector<QueryParam> &params, std::string_view key)
{
    for (const QueryParam &param : params) {
        if (param.key == key) {
            return param.value;
        }
    }
    return {};
}

} // namespace revlm
