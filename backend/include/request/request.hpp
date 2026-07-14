#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <odb/nullable.hxx>

#include "models/models.hpp"
#include "store/database.hpp"
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
    Request(Model model, int input_tokens, int output_tokens, int cache_read_tokens, int cache_creation_1h_tokens,
            int cache_creation_5m_tokens, double tier_multiplier = 1.0, double channel_multiplier = 1.0)
        : model(std::move(model))
        , input_tokens(input_tokens)
        , output_tokens(output_tokens)
        , cache_read_tokens(cache_read_tokens)
        , cache_creation_1h_tokens(cache_creation_1h_tokens)
        , cache_creation_5m_tokens(cache_creation_5m_tokens)
        , tier_multiplier(tier_multiplier)
        , channel_multiplier(channel_multiplier)
    {
    }

#pragma db transient
    Model model;

#pragma db id
    long long id = 0;
    std::string time;
#pragma db transient
    std::string date; // YYYY-MM-DD UTC, derived from time
    long long user_id = 0;
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
#pragma db transient
    bool statue = false;
    std::string status;

#pragma db column("model")
    odb::nullable<std::string> model_name;

    double solve_price() const;
    bool commit(odb::database &db, std::string_view finished_at);
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
    return (model.input_price * input_tokens / 1000000.0 + model.output_price * output_tokens / 1000000.0 +
            model.cache_read_price * cache_read_tokens / 1000000.0 +
            model.cache_creation_1h_price * cache_creation_1h_tokens / 1000000.0 +
            model.cache_creation_5m_price * cache_creation_5m_tokens / 1000000.0) *
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

inline void hydrate_request_model(Request &req)
{
    const std::string name = req.model_name.null() ? req.model.name : *req.model_name;
    if (name.empty()) {
        return;
    }
    const std::vector<Model> &models = ModelManager::instance().models();
    const auto it = std::find_if(models.begin(), models.end(), [&](const Model &m) { return m.name == name; });
    if (it != models.end()) {
        req.model = *it;
    }
}

namespace request_detail
{

inline std::string format_multiplier(double value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", value);
    return std::string{ buf };
}

inline long long parse_i64(const std::optional<std::string> &value)
{
    if (!value.has_value()) {
        return 0;
    }
    try {
        return std::stoll(trim_ascii(*value));
    } catch (const std::exception &) {
        return 0;
    }
}

inline int parse_int(const std::optional<std::string> &value)
{
    return static_cast<int>(parse_i64(value));
}

inline double parse_double(const std::optional<std::string> &value)
{
    if (!value.has_value() || trim_ascii(*value).empty()) {
        return 0.0;
    }
    try {
        return std::stod(trim_ascii(*value));
    } catch (const std::exception &) {
        return 0.0;
    }
}

inline std::string opt_str(const std::optional<std::string> &value)
{
    return value.has_value() ? *value : "";
}

} // namespace request_detail

inline void set_nullable(odb::nullable<std::string> &field, const std::optional<std::string> &value)
{
    if (value.has_value()) {
        field = *value;
    }
}

inline Request row_to_request(const SqlResultRow &row)
{
    Request req;
    if (row.size() < 23) {
        return req;
    }
    req.id = request_detail::parse_i64(row[0]);
    req.time = request_detail::opt_str(row[1]);
    if (req.time.size() >= 10) {
        req.date = req.time.substr(0, 10);
    }
    set_nullable(req.endpoint, row[2]);
    set_nullable(req.method, row[3]);
    req.status_code = request_detail::parse_int(row[4]);
    req.latency_ms = request_detail::parse_int(row[5]);
    req.first_token_latency_ms = request_detail::parse_int(row[6]);
    set_nullable(req.error_class, row[7]);
    set_nullable(req.error_message, row[8]);
    req.user_id = request_detail::parse_i64(row[9]);
    req.token_id = request_detail::parse_i64(row[10]);
    req.channel_id = request_detail::parse_i64(row[11]);
    req.status = request_detail::opt_str(row[12]);
    req.statue = req.status == "committed";
    set_nullable(req.model_name, row[13]);
    if (!req.model_name.null()) {
        req.model.name = *req.model_name;
    }
    set_nullable(req.service_tier, row[14]);
    req.input_tokens = request_detail::parse_int(row[15]);
    req.cache_read_tokens = request_detail::parse_int(row[16]);
    req.cache_creation_5m_tokens = request_detail::parse_int(row[17]);
    req.cache_creation_1h_tokens = request_detail::parse_int(row[18]);
    req.output_tokens = request_detail::parse_int(row[19]);
    req.tier_multiplier = request_detail::opt_str(row[20]).empty() ? 1.0 : request_detail::parse_double(row[20]);
    req.channel_multiplier = request_detail::opt_str(row[21]).empty() ? 1.0 : request_detail::parse_double(row[21]);
    req.is_stream = request_detail::parse_i64(row[22]) != 0;
    return req;
}

class RequestStore {
public:
    explicit RequestStore(odb::database &db);

    std::vector<Request> list(long long user_id, long long token_id, std::string start, std::string end,
                              std::string model, int limit);
    std::optional<Request> get(long long user_id, long long token_id, long long id);
    std::vector<RequestTotal> totals(long long user_id, long long token_id, std::string start_date,
                                     std::string end_date);
    void apply_committed(const Request &request);

private:
    odb::database &db_;
};

} // namespace revlm
