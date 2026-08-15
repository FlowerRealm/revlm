#pragma once

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <odb/database.hxx>
#include <odb/nullable.hxx>

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

/*
 * Aggregate rollup keyed by (user, token, day). Fed only by core dimensions:
 * request count, final USD and first-token latency. Token/cache columns are
 * protocol-shaped pricing detail that moved into `usage_details` and stopped
 * being something the core aggregates (ADR 0004) -- there is no replacement
 * column here, this table just no longer carries them.
 */
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
    double channel_group_multiplier = 1.0;
    long long channel_id = 0;
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    odb::nullable<std::string> error_message;
    // Raw protocol usage JSON, opaque to the core (ADR 0004 / CONTEXT usage_details).
    // Same mapping as Channel::config_json: plain TEXT, not a nested describe type.
    std::string usage_details = "{}";

#pragma db column("model")
    odb::nullable<std::string> model_name;
    double usd = 0;

    double solve_price() const;
    bool commit(std::string_view finished_at);
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
