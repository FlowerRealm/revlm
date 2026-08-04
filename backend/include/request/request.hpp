#pragma once

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <odb/database.hxx>
#include <odb/nullable.hxx>

#include "util/json.hpp"

namespace revlm
{

#pragma db value
struct RequestTotalId {
#pragma db column("user_id")
    long long user_id = 0;
#pragma db column("token_id")
    long long token_id = 0;
#pragma db column("date")
    std::string date; // YYYY-MM-DD UTC
};

#pragma db object table("request_totals")
class RequestTotal {
public:
#pragma db id column("")
    RequestTotalId id;
    long long requests = 0;
    double usd = 0;
    long long first_token_latency_sum = 0;
};

#pragma db object table("requests")
class Request {
public:
    Request() = default;

#pragma db id
    long long id = 0;
    std::string time;
#pragma db transient
    std::string date; // YYYY-MM-DD UTC, derived from time
    long long user_id = 0;
    odb::nullable<std::string> request_id;
    odb::nullable<std::string> response_id;
    odb::nullable<std::string> endpoint;
    odb::nullable<std::string> method;
    long long token_id = 0;
    odb::nullable<std::string> token_details;
    double channel_group_multiplier = 1.0;
    long long channel_id = 0;
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    odb::nullable<std::string> error_class;
    odb::nullable<std::string> error_message;
    bool is_stream = false;

#pragma db column("model")
    odb::nullable<std::string> model_name;
    double usd = 0;

    double solve_price() const;
    bool commit(std::string_view finished_at);
};

struct PricingBreakdown {
    std::optional<std::string> model_public_id;
    std::optional<std::string> service_tier;
    long long input_tokens_total = 0;
    long long input_tokens_cache_read = 0;
    long long input_tokens_cache_creation = 0;
    long long input_tokens_cache_creation_5m = 0;
    long long input_tokens_cache_creation_1h = 0;
    long long input_tokens_billable = 0;
    long long output_tokens_total = 0;
    double tier_multiplier = 1.0;
    double channel_multiplier = 1.0;
    std::string final_cost_usd = "0.000000";
};

// Protocol token statistics are plugin-owned and live in the raw token_details
// JSON (ADR-0004). The core extracts whatever numeric fields are present for
// display/aggregation; values are protocol-dependent and default to zero.
struct UsageTokens {
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_tokens = 0;
    long long cache_creation_5m_tokens = 0;
    long long cache_creation_1h_tokens = 0;
};

inline UsageTokens usage_tokens(const Request &req)
{
    UsageTokens out;
    if (!req.token_details || req.token_details->empty()) {
        return out;
    }
    const auto parsed = json::parse(*req.token_details);
    if (!parsed.has_value() || !parsed->is_object()) {
        return out;
    }
    const auto usage = (*parsed)["usage"];
    if (!usage.is_object()) {
        return out;
    }
    out.input_tokens = usage["input_tokens"].as_int64().value_or(0);
    out.output_tokens = usage["output_tokens"].as_int64().value_or(0);
    out.cache_read_tokens = usage["cache_read_input_tokens"].as_int64().value_or(0);
    const auto cache_creation = usage["cache_creation"];
    if (cache_creation.is_object()) {
        out.cache_creation_5m_tokens = cache_creation["ephemeral_5m_input_tokens"].as_int64().value_or(0);
        out.cache_creation_1h_tokens = cache_creation["ephemeral_1h_input_tokens"].as_int64().value_or(0);
    }
    return out;
}

struct RequestListFilter {
    std::optional<long long> id;
    std::optional<long long> user_id;
    std::optional<long long> token_id;
    std::optional<long long> channel_id;
    std::optional<std::string> start; // inclusive MySQL datetime UTC
    std::optional<std::string> end_exclusive; // exclusive MySQL datetime UTC
    std::optional<std::string> model_exact;
    std::optional<std::string> model_like;
    std::optional<long long> before_id;
    std::optional<long long> after_id;
    std::vector<long long> user_ids;
    std::vector<long long> channel_ids;
    int limit = 0; // 0 = no LIMIT
    bool order_asc = false;
};

std::string request_timestamp_now();

inline double Request::solve_price() const
{
    return usd;
}

inline std::optional<std::string> normalize_usage_service_tier(const std::optional<std::string> &value)
{
    if (!value.has_value()) {
        return std::nullopt;
    }
    return value;
}

namespace request_detail
{

inline std::string format_multiplier(double value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", value);
    return std::string{ buf };
}

inline std::string price_string(double price)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6f", price);
    return std::string{ buffer };
}

inline std::string decimal_to_string(double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6f", value < 0.0 ? 0.0 : value);
    return std::string{ buffer };
}

} // namespace request_detail

// Per-protocol token statistics live in the plugin-owned token_details JSON
// (ADR-0004). The core does not define a fixed token schema, so the display
// breakdown extracts whatever numeric fields are present and defaults to zero.
inline PricingBreakdown compute_pricing_breakdown(const Request &req)
{
    PricingBreakdown pricing;
    const std::string model_id = req.model_name.null() ? "" : *req.model_name;
    pricing.model_public_id = model_id.empty() ? std::nullopt : std::optional<std::string>{ model_id };

    if (req.token_details && !req.token_details->empty()) {
        const auto parsed = json::parse(*req.token_details);
        if (parsed.has_value() && parsed->is_object()) {
            const auto usage = (*parsed)["usage"];
            const auto input = usage["input_tokens"].as_int64().value_or(0);
            const auto output = usage["output_tokens"].as_int64().value_or(0);
            const auto cached = usage["cache_read_input_tokens"].as_int64().value_or(0);
            const auto cache_5m = usage["cache_creation"].is_object() ?
                                      usage["cache_creation"]["ephemeral_5m_input_tokens"].as_int64().value_or(0) :
                                      0;
            const auto cache_1h = usage["cache_creation"].is_object() ?
                                      usage["cache_creation"]["ephemeral_1h_input_tokens"].as_int64().value_or(0) :
                                      0;
            pricing.input_tokens_total = input;
            pricing.input_tokens_cache_read = cached;
            pricing.input_tokens_cache_creation_5m = cache_5m;
            pricing.input_tokens_cache_creation_1h = cache_1h;
            pricing.input_tokens_cache_creation = cache_5m + cache_1h;
            pricing.output_tokens_total = output;
            pricing.input_tokens_billable = std::max(0LL, input - cached - cache_5m - cache_1h);
            const auto service_tier = usage["service_tier"].as_string();
            if (service_tier.has_value() && !service_tier->empty()) {
                pricing.service_tier = *service_tier;
            }
        }
    }

    pricing.channel_multiplier = req.channel_group_multiplier;
    pricing.final_cost_usd = request_detail::decimal_to_string(req.solve_price());
    return pricing;
}

class RequestStore {
public:
    RequestStore();

    std::vector<Request> query(const RequestListFilter &filter);
    std::vector<Request> list(long long user_id, long long token_id, std::string start, std::string end,
                              std::string model, int limit);
    std::optional<Request> get(long long user_id, long long token_id, long long id);
    std::optional<Request> get_by_id(long long id);
    std::vector<RequestTotal> totals(long long user_id, long long token_id, std::string start_date,
                                     std::string end_date);
    void apply_total(const Request &request);

private:
    odb::database &db_;
};

} // namespace revlm
