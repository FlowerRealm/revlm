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

#include "models/models.hpp"
#include "store/mysql.hpp"
#include "util/strings.hpp"

namespace revlm
{

// 按日聚合形状；挂在某 Token 的 RequestStore 内，自身不存 user_id/token_id。
class RequestTotal {
public:
    std::string date; // YYYY-MM-DD UTC
    long long requests = 0;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_tokens = 0;
    long long cache_creation_tokens = 0;
    long long tokens = 0; // input+output+cache_read+cache_creation
    double usd = 0;
    long long first_token_latency_sum = 0;
};

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

    Model model;
    long long id = 0;
    std::string time;
    std::string date; // YYYY-MM-DD UTC，commit/装载时填好
    long long user_id = 0;
    std::string endpoint;
    std::string method;
    long long token_id = 0;
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;
    int cache_creation_1h_tokens = 0;
    int cache_creation_5m_tokens = 0;
    double tier_multiplier = 1.0;
    std::string service_tier;
    double channel_multiplier = 1.0;
    long long channel_id = 0;
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    std::string error_class;
    std::string error_message;
    bool is_stream = false;
    bool statue = false;
    std::string status;

    double solve_price() const;
    // 写入 requests，再经 UserStore→TokenStore→RequestStore::apply_committed
    bool commit(MysqlConnection &conn, std::string_view finished_at);
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
    const std::string &name = req.model.name;
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

inline Request row_to_request(const MysqlResultRow &row)
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
    req.endpoint = request_detail::opt_str(row[2]);
    req.method = request_detail::opt_str(row[3]);
    req.status_code = request_detail::parse_int(row[4]);
    req.latency_ms = request_detail::parse_int(row[5]);
    req.first_token_latency_ms = request_detail::parse_int(row[6]);
    req.error_class = request_detail::opt_str(row[7]);
    req.error_message = request_detail::opt_str(row[8]);
    req.user_id = request_detail::parse_i64(row[9]);
    req.token_id = request_detail::parse_i64(row[10]);
    req.channel_id = request_detail::parse_i64(row[11]);
    req.status = request_detail::opt_str(row[12]);
    req.statue = req.status == "committed";
    req.model.name = request_detail::opt_str(row[13]);
    req.service_tier = request_detail::opt_str(row[14]);
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

// 按 Token 作用域的请求 Store。user_id_/token_id_ 由父 Token 槽位注入，公开 API 不再接收。
class RequestStore {
public:
    // 只装「本 token」的 request_totals。
    void reload(MysqlConnection &conn);

    std::vector<Request> list(std::string start, std::string end, std::string model, int limit);
    std::optional<Request> get(long long id);
    std::vector<RequestTotal> totals(std::string start_date, std::string end_date);
    void apply_committed(const Request &request);

    bool bound() const
    {
        return user_id_ > 0 && token_id_ > 0;
    }

private:
    friend class TokenStore;
    RequestStore() = default;

    void bind(long long user_id, long long token_id)
    {
        user_id_ = user_id;
        token_id_ = token_id;
    }

    void load_from_db(MysqlConnection &conn);
    bool align_request_total_to_db(std::string_view date);
    RequestTotal &total_for_write(std::string_view date);
    void accumulate_into_total(RequestTotal &total, const Request &request);

    MysqlConnection *conn_ = nullptr;
    long long user_id_ = 0;
    long long token_id_ = 0;
    std::vector<RequestTotal> totals_;
};

inline void RequestStore::reload(MysqlConnection &conn)
{
    conn_ = &conn;
    if (!bound()) {
        totals_.clear();
        return;
    }
    load_from_db(conn);
}

inline void RequestStore::load_from_db(MysqlConnection &conn)
{
    totals_.clear();
    const auto rows = conn.query_rows(
        "SELECT date,requests,input_tokens,output_tokens,cache_read_tokens,cache_creation_tokens,tokens,usd,"
        "first_token_latency_sum FROM request_totals WHERE user_id=" +
        std::to_string(user_id_) + " AND token_id=" + std::to_string(token_id_));
    for (const auto &row : rows) {
        if (row.size() < 9) {
            continue;
        }
        RequestTotal total;
        total.date = request_detail::opt_str(row[0]);
        total.requests = request_detail::parse_i64(row[1]);
        total.input_tokens = request_detail::parse_i64(row[2]);
        total.output_tokens = request_detail::parse_i64(row[3]);
        total.cache_read_tokens = request_detail::parse_i64(row[4]);
        total.cache_creation_tokens = request_detail::parse_i64(row[5]);
        total.tokens = request_detail::parse_i64(row[6]);
        total.usd = request_detail::parse_double(row[7]);
        total.first_token_latency_sum = request_detail::parse_i64(row[8]);
        totals_.push_back(std::move(total));
    }
}

inline std::vector<Request> RequestStore::list(std::string start, std::string end, std::string model, int limit)
{
    if (conn_ == nullptr || !bound()) {
        return {};
    }
    if (limit <= 0) {
        limit = 50;
    }
    if (limit > 200) {
        limit = 200;
    }
    std::string sql = "SELECT id,time,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                      "error_class,error_message,user_id,token_id,channel_id,status,model,service_tier,"
                      "input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
                      "output_tokens,tier_multiplier,channel_multiplier,is_stream FROM requests WHERE user_id=" +
                      std::to_string(user_id_) + " AND token_id=" + std::to_string(token_id_) +
                      " AND time>=" + conn_->quote(start) + " AND time<" + conn_->quote(end);
    if (!model.empty()) {
        sql += " AND model=" + conn_->quote(model);
    }
    sql += " ORDER BY id DESC LIMIT " + std::to_string(limit);
    const auto rows = conn_->query_rows(sql);
    std::vector<Request> out;
    out.reserve(rows.size());
    for (const auto &row : rows) {
        Request req = row_to_request(row);
        hydrate_request_model(req);
        out.push_back(std::move(req));
    }
    return out;
}

inline std::optional<Request> RequestStore::get(long long id)
{
    if (conn_ == nullptr || !bound()) {
        return std::nullopt;
    }
    const std::string sql = "SELECT id,time,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                            "error_class,error_message,user_id,token_id,channel_id,status,model,service_tier,"
                            "input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
                            "output_tokens,tier_multiplier,channel_multiplier,is_stream FROM requests WHERE id=" +
                            std::to_string(id) + " AND user_id=" + std::to_string(user_id_) +
                            " AND token_id=" + std::to_string(token_id_) + " LIMIT 1";
    const auto rows = conn_->query_rows(sql);
    if (rows.empty()) {
        return std::nullopt;
    }
    Request req = row_to_request(rows.front());
    hydrate_request_model(req);
    return req;
}

inline std::vector<RequestTotal> RequestStore::totals(std::string start_date, std::string end_date)
{
    std::vector<RequestTotal> out;
    for (const RequestTotal &row : totals_) {
        if (row.date >= start_date && row.date <= end_date) {
            out.push_back(row);
        }
    }
    return out;
}

inline RequestTotal &RequestStore::total_for_write(std::string_view date)
{
    for (RequestTotal &row : totals_) {
        if (row.date == date) {
            return row;
        }
    }
    RequestTotal created;
    created.date = std::string{ date };
    totals_.push_back(std::move(created));
    return totals_.back();
}

inline void RequestStore::accumulate_into_total(RequestTotal &total, const Request &request)
{
    total.requests += 1;
    total.input_tokens += std::max(request.input_tokens, 0);
    total.output_tokens += std::max(request.output_tokens, 0);
    total.cache_read_tokens += std::max(request.cache_read_tokens, 0);
    total.cache_creation_tokens +=
        std::max(request.cache_creation_5m_tokens, 0) + std::max(request.cache_creation_1h_tokens, 0);
    total.tokens = total.input_tokens + total.output_tokens + total.cache_read_tokens + total.cache_creation_tokens;
    total.usd += request.solve_price();
    if (request.first_token_latency_ms > 0) {
        total.first_token_latency_sum += request.first_token_latency_ms;
    }
}

inline bool RequestStore::align_request_total_to_db(std::string_view date)
{
    if (conn_ == nullptr || !bound()) {
        return false;
    }
    const RequestTotal *found = nullptr;
    for (const RequestTotal &row : totals_) {
        if (row.date == date) {
            found = &row;
            break;
        }
    }
    if (found == nullptr) {
        conn_->exec("DELETE FROM request_totals WHERE user_id=" + std::to_string(user_id_) +
                    " AND token_id=" + std::to_string(token_id_) + " AND date=" + conn_->quote(date));
        return true;
    }
    char usd_buf[64];
    std::snprintf(usd_buf, sizeof(usd_buf), "%.6f", found->usd);
    conn_->exec("INSERT INTO request_totals(user_id,token_id,date,requests,input_tokens,output_tokens,"
                "cache_read_tokens,cache_creation_tokens,tokens,usd,first_token_latency_sum) VALUES(" +
                std::to_string(user_id_) + "," + std::to_string(token_id_) + "," + conn_->quote(date) + "," +
                std::to_string(found->requests) + "," + std::to_string(found->input_tokens) + "," +
                std::to_string(found->output_tokens) + "," + std::to_string(found->cache_read_tokens) + "," +
                std::to_string(found->cache_creation_tokens) + "," + std::to_string(found->tokens) + "," + usd_buf +
                "," + std::to_string(found->first_token_latency_sum) +
                ") ON DUPLICATE KEY UPDATE requests=VALUES(requests),input_tokens=VALUES(input_tokens),"
                "output_tokens=VALUES(output_tokens),cache_read_tokens=VALUES(cache_read_tokens),"
                "cache_creation_tokens=VALUES(cache_creation_tokens),tokens=VALUES(tokens),usd=VALUES(usd),"
                "first_token_latency_sum=VALUES(first_token_latency_sum)");
    return true;
}

inline void RequestStore::apply_committed(const Request &request)
{
    if (conn_ == nullptr || !bound() || request.date.empty()) {
        return;
    }
    // 以父槽位为准；Request 上的 user/token 若不一致则忽略其值
    RequestTotal &total = total_for_write(request.date);
    accumulate_into_total(total, request);
    (void)align_request_total_to_db(request.date);
}

} // namespace revlm
