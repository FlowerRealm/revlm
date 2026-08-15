#pragma once

#include <boost/describe.hpp>
#include <boost/json.hpp>
#include <boost/version.hpp>

#include <odb/nullable.hxx>
#include <optional>
#include <utility>

#include "users/users.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "request/request.hpp"
#include "users/tokens.hpp"
#include "util/json.hpp"

// ---------- 字段注册（白名单，敏感字段不列入） ----------

namespace revlm
{

BOOST_DESCRIBE_STRUCT(User, (), (id, email, username, role, status, balance_usd))
BOOST_DESCRIBE_STRUCT(UserToken, (), (id, user_id, name, status, channel_group_id))
BOOST_DESCRIBE_STRUCT(Channel, (), (id, type, name, status, priority, base_url, api_key, price_multiplier, config_json))
BOOST_DESCRIBE_STRUCT(ChannelGroup, (), (id, name, description, price_multiplier, status, type))
BOOST_DESCRIBE_STRUCT(Request, (),
                      (id, time, user_id, request_id, response_id, endpoint, method, token_id, channel_group_multiplier,
                       channel_id, status_code, latency_ms, first_token_latency_ms, error_message, usage_details,
                       model_name, usd))

} // namespace revlm

// ---------- 描述字段里出现的包装类型 ----------
//
// `revlm::to_json` / `revlm::strict_from` 走 Boost.JSON 的 describe 集成，它认识标准容器
// 和字符串，但不认识 ODB 的 nullable。下面这对 tag_invoke 由 ADL 在 namespace odb 中找到，
// 把 nullable 映射为 JSON 的 null 或其内层值。

namespace odb
{

template <class T> void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, const nullable<T> &v)
{
    if (v.null()) {
        jv = nullptr;
    } else {
        jv = boost::json::value_from(*v);
    }
}

template <class T> nullable<T> tag_invoke(boost::json::value_to_tag<nullable<T>>, const boost::json::value &jv)
{
    if (jv.is_null()) {
        return nullable<T>{};
    }
    return nullable<T>{ boost::json::value_to<T>(jv) };
}

} // namespace odb

// Boost.JSON 从 1.84 起原生识别 std::optional（is_optional_like）。Ubuntu 24.04 的
// libboost-json-dev 仍是 1.83，因此在更早的版本上自行补一对 tag_invoke；不能定义在
// namespace std 里，只能靠 tag 类型把 ADL 引到 boost::json。两侧同时定义会二义。
#if BOOST_VERSION < 108400
namespace boost::json
{

template <class T> void tag_invoke(value_from_tag, value &jv, const std::optional<T> &v)
{
    if (v.has_value()) {
        jv = value_from(*v);
    } else {
        jv = nullptr;
    }
}

template <class T> std::optional<T> tag_invoke(value_to_tag<std::optional<T>>, const value &jv)
{
    if (jv.is_null()) {
        return std::nullopt;
    }
    return value_to<T>(jv);
}

} // namespace boost::json
#endif
