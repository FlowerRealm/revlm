#pragma once

#include <boost/describe.hpp>
#include <boost/json.hpp>
#include <boost/mp11.hpp>

#include <iterator>
#include <odb/nullable.hxx>
#include <optional>
#include <string>
#include <type_traits>

#include "auth/users.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "request/request.hpp"
#include "server/tokens.hpp"

namespace revlm
{

// ---------- 字段注册（白名单，敏感字段不列入） ----------

BOOST_DESCRIBE_STRUCT(User, (), (id, email, username, role, status, balance_usd))
BOOST_DESCRIBE_STRUCT(UserToken, (), (id, user_id, name, status, channel_id))
BOOST_DESCRIBE_STRUCT(Channel, (), (id, type, name, status, priority, base_url, api_key, price_multiplier))
BOOST_DESCRIBE_STRUCT(ChannelGroup, (), (id, name, description, price_multiplier, status))
BOOST_DESCRIBE_STRUCT(PricingBreakdown, (),
                      (model_public_id, model_found, owned_by, service_tier, pricing_kind, input_tokens_total,
                       input_tokens_cache_read, input_tokens_cache_creation, input_tokens_cache_creation_5m,
                       input_tokens_cache_creation_1h, input_tokens_billable, output_tokens_total, input_usd_per_1m,
                       output_usd_per_1m, cache_read_usd_per_1m, cache_creation_5m_usd_per_1m,
                       cache_creation_1h_usd_per_1m, input_cost_usd, output_cost_usd, cache_read_cost_usd,
                       cache_creation_cost_usd, cache_creation_5m_cost_usd, cache_creation_1h_cost_usd, base_cost_usd,
                       tier_multiplier, channel_multiplier, final_cost_usd))
BOOST_DESCRIBE_STRUCT(Request, (),
                      (id, time, user_id, request_id, response_id, endpoint, method, token_id, input_tokens,
                       output_tokens, cache_read_tokens, cache_creation_1h_tokens, cache_creation_5m_tokens,
                       tier_multiplier, service_tier, channel_multiplier, channel_id, status_code, latency_ms,
                       first_token_latency_ms, error_class, error_message, is_stream, status, model_name))

// ---------- 泛型 to_json ----------

template <class T, class Md = boost::describe::describe_members<T, boost::describe::mod_public>>
boost::json::object to_json(const T &v)
{
    boost::json::object o;
    boost::mp11::mp_for_each<Md>([&](auto D) {
        const auto &fv = v.*D.pointer;
        using FT = std::decay_t<decltype(fv)>;
        if constexpr (requires { fv.null(); }) {
            o[D.name] = fv.null() ? boost::json::value(nullptr) : boost::json::value(*fv);
        } else if constexpr (requires {
                                 fv.has_value();
                                 *fv;
                             }) {
            o[D.name] = fv.has_value() ? boost::json::value(*fv) : boost::json::value(nullptr);
        } else if constexpr (!std::is_same_v<FT, std::string> && requires {
                                 std::begin(fv);
                                 std::end(fv);
                             }) {
            boost::json::array a;
            for (const auto &e : fv) {
                a.push_back(boost::json::value(e));
            }
            o[D.name] = std::move(a);
        } else {
            o[D.name] = fv;
        }
    });
    return o;
}

} // namespace revlm
