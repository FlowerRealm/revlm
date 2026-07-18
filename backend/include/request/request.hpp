#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <odb/database.hxx>
#include <odb/nullable.hxx>

#include "models/models.hpp"
#include "util/strings.hpp"

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
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_tokens = 0;
    long long cache_creation_tokens = 0;
    long long tokens = 0; // input+output+cache_read+cache_creation
    double usd = 0;
    long long first_token_latency_sum = 0;
};

#pragma db object table("requests")
class Request {
public:
    Request() = default;

#pragma db transient
    const Model *pricing_model = nullptr;

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
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;
    int cache_creation_1h_tokens = 0;
    int cache_creation_5m_tokens = 0;
    double tier_multiplier = 1.0;
    odb::nullable<std::string> service_tier;
    double channel_multiplier = 1.0;
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
    bool model_found = false;
    std::optional<std::string> owned_by;
    std::optional<std::string> service_tier;
    std::string pricing_kind = "base";
    long long input_tokens_total = 0;
    long long input_tokens_cache_read = 0;
    long long input_tokens_cache_creation = 0;
    long long input_tokens_cache_creation_5m = 0;
    long long input_tokens_cache_creation_1h = 0;
    long long input_tokens_billable = 0;
    long long output_tokens_total = 0;
    std::string input_usd_per_1m = "0.000000";
    std::string output_usd_per_1m = "0.000000";
    std::string cache_read_usd_per_1m = "0.000000";
    std::string cache_creation_5m_usd_per_1m = "0.000000";
    std::string cache_creation_1h_usd_per_1m = "0.000000";
    std::string input_cost_usd = "0.000000";
    std::string output_cost_usd = "0.000000";
    std::string cache_read_cost_usd = "0.000000";
    std::string cache_creation_cost_usd = "0.000000";
    std::string cache_creation_5m_cost_usd = "0.000000";
    std::string cache_creation_1h_cost_usd = "0.000000";
    std::string base_cost_usd = "0.000000";
    double tier_multiplier = 1.0;
    double channel_multiplier = 1.0;
    std::string final_cost_usd = "0.000000";
};

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

inline std::string request_timestamp_now()
{
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string{ buffer };
}

inline double Request::solve_price() const
{
    if (pricing_model == nullptr) {
        return usd;
    }
    return (pricing_model->input_price * input_tokens / 1000000.0 +
            pricing_model->output_price * output_tokens / 1000000.0 +
            pricing_model->cache_read_price * cache_read_tokens / 1000000.0 +
            pricing_model->cache_creation_1h_price * cache_creation_1h_tokens / 1000000.0 +
            pricing_model->cache_creation_5m_price * cache_creation_5m_tokens / 1000000.0) *
           tier_multiplier * channel_multiplier;
}

inline std::string normalize_usage_service_tier(std::string_view raw)
{
    std::string value = lowercase_ascii(trim_ascii(raw));
    if (value == "fast" || value == "priority") {
        return "priority";
    }
    if (value == "flex") {
        return "flex";
    }
    if (value.empty() || value == "default" || value == "auto" || value == "standard") {
        return "default";
    }
    return value;
}

inline std::optional<std::string> normalize_usage_service_tier(const std::optional<std::string> &value)
{
    if (!value.has_value()) {
        return std::nullopt;
    }
    return normalize_usage_service_tier(std::string_view{ *value });
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

inline void hydrate_request_model(Request &req)
{
    if (req.pricing_model != nullptr && (req.model_name.null() || req.model_name->empty())) {
        req.model_name = req.pricing_model->name;
    }
}

inline PricingBreakdown compute_pricing_breakdown(const Request &req)
{
    Request hydrated = req;
    hydrate_request_model(hydrated);

    PricingBreakdown pricing;
    const std::string model_id = trim_ascii(hydrated.model_name.null() ? "" : *hydrated.model_name);
    const Model *builtin = hydrated.pricing_model;
    const bool found = builtin != nullptr;
    pricing.model_public_id = model_id.empty() ? std::nullopt : std::optional<std::string>{ model_id };
    pricing.model_found = found;
    pricing.owned_by = found ? std::optional<std::string>{ builtin->owned_by } : std::optional<std::string>{ "openai" };
    pricing.service_tier = hydrated.service_tier.null() || hydrated.service_tier->empty() ?
                               std::nullopt :
                               std::optional<std::string>{ *hydrated.service_tier };
    pricing.input_tokens_total = hydrated.input_tokens;
    pricing.input_tokens_cache_read = hydrated.cache_read_tokens;
    pricing.input_tokens_cache_creation_5m = hydrated.cache_creation_5m_tokens;
    pricing.input_tokens_cache_creation_1h = hydrated.cache_creation_1h_tokens;
    pricing.input_tokens_cache_creation = hydrated.cache_creation_5m_tokens + hydrated.cache_creation_1h_tokens;
    pricing.output_tokens_total = hydrated.output_tokens;
    pricing.input_tokens_billable =
        std::max(0, hydrated.input_tokens - hydrated.cache_read_tokens - hydrated.cache_creation_5m_tokens -
                        hydrated.cache_creation_1h_tokens);
    pricing.input_usd_per_1m = found ? request_detail::price_string(builtin->input_price) : "0.000000";
    pricing.output_usd_per_1m = found ? request_detail::price_string(builtin->output_price) : "0.000000";
    pricing.cache_read_usd_per_1m = found ? request_detail::price_string(builtin->cache_read_price) : "0.000000";
    pricing.cache_creation_5m_usd_per_1m = found ? request_detail::price_string(builtin->cache_creation_5m_price) :
                                                   "0.000000";
    pricing.cache_creation_1h_usd_per_1m = found ? request_detail::price_string(builtin->cache_creation_1h_price) :
                                                   "0.000000";

    const double input_rate = (found ? builtin->input_price : 0.0) / 1000000.0;
    const double output_rate = (found ? builtin->output_price : 0.0) / 1000000.0;
    const double cache_read_rate = (found ? builtin->cache_read_price : 0.0) / 1000000.0;
    const double cache_create_5m_rate = (found ? builtin->cache_creation_5m_price : 0.0) / 1000000.0;
    const double cache_create_1h_rate = (found ? builtin->cache_creation_1h_price : 0.0) / 1000000.0;
    const double input_cost = static_cast<double>(pricing.input_tokens_billable) * input_rate;
    const double output_cost = static_cast<double>(pricing.output_tokens_total) * output_rate;
    const double cache_read_cost = static_cast<double>(pricing.input_tokens_cache_read) * cache_read_rate;
    const double cache_create_5m_cost =
        static_cast<double>(pricing.input_tokens_cache_creation_5m) * cache_create_5m_rate;
    const double cache_create_1h_cost =
        static_cast<double>(pricing.input_tokens_cache_creation_1h) * cache_create_1h_rate;
    const double cache_create_total_cost = cache_create_5m_cost + cache_create_1h_cost;

    pricing.input_cost_usd = request_detail::decimal_to_string(input_cost);
    pricing.output_cost_usd = request_detail::decimal_to_string(output_cost);
    pricing.cache_read_cost_usd = request_detail::decimal_to_string(cache_read_cost);
    pricing.cache_creation_cost_usd = request_detail::decimal_to_string(cache_create_total_cost);
    pricing.cache_creation_5m_cost_usd = request_detail::decimal_to_string(cache_create_5m_cost);
    pricing.cache_creation_1h_cost_usd = request_detail::decimal_to_string(cache_create_1h_cost);
    pricing.base_cost_usd =
        request_detail::decimal_to_string(input_cost + output_cost + cache_read_cost + cache_create_total_cost);
    pricing.tier_multiplier = hydrated.tier_multiplier;
    pricing.channel_multiplier = hydrated.channel_multiplier;
    pricing.final_cost_usd = request_detail::decimal_to_string(hydrated.solve_price());
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
