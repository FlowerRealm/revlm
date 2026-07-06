#include "usage/usage_charge.hpp"

#include "models/quota.hpp"
#include "util/json_util.hpp"

#include <algorithm>
#include <cstdio>

namespace revlm
{
namespace
{

void take_max_token(int &dst, const std::optional<long long> &src)
{
    if (!src.has_value()) {
        return;
    }
    dst = std::max(dst, static_cast<int>(*src));
}

} // namespace

void absorb_usage_object(Request &req, std::string_view body)
{
    const std::optional<std::string> usage_body = extract_json_object_field(body, "usage");
    if (!usage_body.has_value()) {
        return;
    }
    take_max_token(req.input_tokens, parse_json_int_field(*usage_body, "input_tokens"));
    take_max_token(req.output_tokens, parse_json_int_field(*usage_body, "output_tokens"));
    take_max_token(req.cache_read_tokens, parse_json_int_field(*usage_body, "cache_read_input_tokens"));
    take_max_token(req.cache_creation_5m_tokens, parse_json_int_field(*usage_body, "cache_creation_input_tokens"));
    take_max_token(req.cache_creation_1h_tokens, parse_json_int_field(*usage_body, "cache_creation_1h_input_tokens"));
    if (req.input_tokens == 0) {
        take_max_token(req.input_tokens, parse_json_int_field(*usage_body, "prompt_tokens"));
    }
    if (req.output_tokens == 0) {
        take_max_token(req.output_tokens, parse_json_int_field(*usage_body, "completion_tokens"));
    }
}

void fill_payload_from_request(UsageCommitPayload &payload, const Request &req)
{
    payload.input_tokens = req.input_tokens;
    payload.output_tokens = req.output_tokens;
    payload.cache_read_input_tokens = req.cache_read_tokens;
    payload.cache_creation_input_tokens = req.cache_creation_5m_tokens;
    payload.cache_creation_1h_input_tokens = req.cache_creation_1h_tokens;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", req.solve_price());
    payload.committed_usd = buf;
    std::snprintf(buf, sizeof(buf), "%.6f", req.channel_multiplier);
    payload.price_multiplier_group = buf;
    std::snprintf(buf, sizeof(buf), "%.6f", req.tier_multiplier * req.channel_multiplier);
    payload.price_multiplier = buf;
}

void charge_request(MysqlConnection &conn, Request &billing_request, UsageCommitPayload &payload)
{
    Quota quota(conn);
    quota.charge(billing_request);
    fill_payload_from_request(payload, billing_request);
    payload.balance_debited = true;
}

} // namespace revlm
