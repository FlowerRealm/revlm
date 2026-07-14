#include "request/request.hpp"

#include "store/database.hpp"
#include "revlm_entities-odb.hxx"

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>

namespace revlm
{

bool Request::commit(odb::database &db, std::string_view finished_at)
{
    if (id <= 0 || user_id <= 0 || token_id <= 0) {
        return false;
    }

    ScopedTransaction t(db);

    const auto existing =
        sql_query_one(db, "SELECT COUNT(*) FROM requests WHERE id=" + std::to_string(id));
    if (existing && std::stoll(*existing) > 0) {
        t.commit();
        return true;
    }

    const std::string occurred_at = !trim_ascii(time).empty()        ? trim_ascii(time) :
                                    !trim_ascii(finished_at).empty() ? trim_ascii(finished_at) :
                                                                       request_timestamp_now();
    time = occurred_at;
    if (date.empty() && occurred_at.size() >= 10) {
        date = occurred_at.substr(0, 10);
    }
    status = "committed";
    statue = true;

    if (!model.name.empty()) {
        model_name = model.name;
    }

    sql_exec(db,
             "INSERT INTO requests("
             "id,time,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
             "error_class,error_message,user_id,token_id,channel_id,status,model,service_tier,"
             "input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
             "output_tokens,tier_multiplier,channel_multiplier,is_stream"
             ") VALUES(" +
                 std::to_string(id) + "," + sql_quote(db, occurred_at) + "," + sql_nullable(db, endpoint) + "," +
                 sql_nullable(db, method) + "," + std::to_string(std::max(status_code, 0)) + "," +
                 std::to_string(std::max(latency_ms, 0)) + "," + std::to_string(std::max(first_token_latency_ms, 0)) +
                 "," + sql_nullable(db, error_class) + "," + sql_nullable(db, error_message) + "," +
                 std::to_string(user_id) + "," + std::to_string(token_id) + "," + std::to_string(channel_id) + "," +
                 sql_quote(db, status) + "," + sql_nullable(db, model_name) + "," + sql_nullable(db, service_tier) +
                 "," + std::to_string(std::max(input_tokens, 0)) + "," +
                 std::to_string(std::max(cache_read_tokens, 0)) + "," +
                 std::to_string(std::max(cache_creation_5m_tokens, 0)) + "," +
                 std::to_string(std::max(cache_creation_1h_tokens, 0)) + "," +
                 std::to_string(std::max(output_tokens, 0)) + "," +
                 request_detail::format_multiplier(tier_multiplier) + "," +
                 request_detail::format_multiplier(channel_multiplier) + "," + std::to_string(is_stream ? 1 : 0) +
                 ")");

    hydrate_request_model(*this);
    const int cache_creation =
        std::max(cache_creation_5m_tokens, 0) + std::max(cache_creation_1h_tokens, 0);
    const int total_tokens =
        std::max(input_tokens, 0) + std::max(output_tokens, 0) + std::max(cache_read_tokens, 0) + cache_creation;
    char usd_buf[64];
    std::snprintf(usd_buf, sizeof(usd_buf), "%.6f", solve_price());
    const int ftl = std::max(first_token_latency_ms, 0);

    sql_exec(db,
             "INSERT INTO request_totals(user_id,token_id,date,requests,input_tokens,output_tokens,"
             "cache_read_tokens,cache_creation_tokens,tokens,usd,first_token_latency_sum) VALUES(" +
                 std::to_string(user_id) + "," + std::to_string(token_id) + "," + sql_quote(db, date) + ",1," +
                 std::to_string(std::max(input_tokens, 0)) + "," + std::to_string(std::max(output_tokens, 0)) + "," +
                 std::to_string(std::max(cache_read_tokens, 0)) + "," + std::to_string(cache_creation) + "," +
                 std::to_string(total_tokens) + "," + usd_buf + "," + std::to_string(ftl) +
                 ") ON DUPLICATE KEY UPDATE "
                 "requests=requests+1,"
                 "input_tokens=input_tokens+" +
                 std::to_string(std::max(input_tokens, 0)) +
                 ","
                 "output_tokens=output_tokens+" +
                 std::to_string(std::max(output_tokens, 0)) +
                 ","
                 "cache_read_tokens=cache_read_tokens+" +
                 std::to_string(std::max(cache_read_tokens, 0)) +
                 ","
                 "cache_creation_tokens=cache_creation_tokens+" +
                 std::to_string(cache_creation) +
                 ","
                 "tokens=tokens+" +
                 std::to_string(total_tokens) +
                 ","
                 "usd=usd+" +
                 usd_buf +
                 ","
                 "first_token_latency_sum=first_token_latency_sum+" +
                 std::to_string(ftl));

    t.commit();
    return true;
}

RequestStore::RequestStore(odb::database &db)
    : db_(db)
{
}

std::vector<Request> RequestStore::list(long long user_id, long long token_id, std::string start, std::string end,
                                        std::string model, int limit)
{
    if (user_id <= 0 || token_id <= 0) {
        return {};
    }
    if (limit <= 0) {
        limit = 50;
    }
    if (limit > 200) {
        limit = 200;
    }

    ScopedTransaction t(db_);
    std::string sql = "SELECT id,time,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                      "error_class,error_message,user_id,token_id,channel_id,status,model,service_tier,"
                      "input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
                      "output_tokens,tier_multiplier,channel_multiplier,is_stream FROM requests WHERE user_id=" +
                      std::to_string(user_id) + " AND token_id=" + std::to_string(token_id) +
                      " AND time>=" + sql_quote(db_, start) + " AND time<" + sql_quote(db_, end);
    if (!model.empty()) {
        sql += " AND model=" + sql_quote(db_, model);
    }
    sql += " ORDER BY id DESC LIMIT " + std::to_string(limit);
    const auto rows = sql_query_rows(db_, sql);
    t.commit();

    std::vector<Request> out;
    out.reserve(rows.size());
    for (const auto &row : rows) {
        Request req = row_to_request(row);
        hydrate_request_model(req);
        out.push_back(std::move(req));
    }
    return out;
}

std::optional<Request> RequestStore::get(long long user_id, long long token_id, long long id)
{
    if (user_id <= 0 || token_id <= 0) {
        return std::nullopt;
    }

    ScopedTransaction t(db_);
    const std::string sql = "SELECT id,time,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                            "error_class,error_message,user_id,token_id,channel_id,status,model,service_tier,"
                            "input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
                            "output_tokens,tier_multiplier,channel_multiplier,is_stream FROM requests WHERE id=" +
                            std::to_string(id) + " AND user_id=" + std::to_string(user_id) +
                            " AND token_id=" + std::to_string(token_id) + " LIMIT 1";
    const auto rows = sql_query_rows(db_, sql);
    t.commit();

    if (rows.empty()) {
        return std::nullopt;
    }
    Request req = row_to_request(rows.front());
    hydrate_request_model(req);
    return req;
}

std::vector<RequestTotal> RequestStore::totals(long long user_id, long long token_id, std::string start_date,
                                               std::string end_date)
{
    if (user_id <= 0 || token_id <= 0) {
        return {};
    }

    ScopedTransaction t(db_);
    const auto rows = sql_query_rows(
        db_, "SELECT date,requests,input_tokens,output_tokens,cache_read_tokens,cache_creation_tokens,tokens,usd,"
             "first_token_latency_sum FROM request_totals WHERE user_id=" +
                 std::to_string(user_id) + " AND token_id=" + std::to_string(token_id) +
                 " AND date>=" + sql_quote(db_, start_date) + " AND date<=" + sql_quote(db_, end_date));
    t.commit();

    std::vector<RequestTotal> out;
    for (const auto &row : rows) {
        if (row.size() < 9) {
            continue;
        }
        RequestTotal total;
        total.id.user_id = user_id;
        total.id.token_id = token_id;
        total.id.date = request_detail::opt_str(row[0]);
        total.requests = request_detail::parse_i64(row[1]);
        total.input_tokens = request_detail::parse_i64(row[2]);
        total.output_tokens = request_detail::parse_i64(row[3]);
        total.cache_read_tokens = request_detail::parse_i64(row[4]);
        total.cache_creation_tokens = request_detail::parse_i64(row[5]);
        total.tokens = request_detail::parse_i64(row[6]);
        total.usd = request_detail::parse_double(row[7]);
        total.first_token_latency_sum = request_detail::parse_i64(row[8]);
        out.push_back(std::move(total));
    }
    return out;
}

void RequestStore::apply_committed(const Request &request)
{
    if (request.user_id <= 0 || request.token_id <= 0 || request.date.empty()) {
        return;
    }
    const int cache_creation =
        std::max(request.cache_creation_5m_tokens, 0) + std::max(request.cache_creation_1h_tokens, 0);
    const int total_tokens =
        std::max(request.input_tokens, 0) + std::max(request.output_tokens, 0) +
        std::max(request.cache_read_tokens, 0) + cache_creation;
    char usd_buf[64];
    std::snprintf(usd_buf, sizeof(usd_buf), "%.6f", request.solve_price());
    const int ftl = std::max(request.first_token_latency_ms, 0);

    ScopedTransaction t(db_);
    sql_exec(db_,
             "INSERT INTO request_totals(user_id,token_id,date,requests,input_tokens,output_tokens,"
             "cache_read_tokens,cache_creation_tokens,tokens,usd,first_token_latency_sum) VALUES(" +
                 std::to_string(request.user_id) + "," + std::to_string(request.token_id) + "," +
                 sql_quote(db_, request.date) + ",1," + std::to_string(std::max(request.input_tokens, 0)) + "," +
                 std::to_string(std::max(request.output_tokens, 0)) + "," +
                 std::to_string(std::max(request.cache_read_tokens, 0)) + "," + std::to_string(cache_creation) + "," +
                 std::to_string(total_tokens) + "," + usd_buf + "," + std::to_string(ftl) +
                 ") ON DUPLICATE KEY UPDATE "
                 "requests=requests+1,"
                 "input_tokens=input_tokens+" +
                 std::to_string(std::max(request.input_tokens, 0)) +
                 ","
                 "output_tokens=output_tokens+" +
                 std::to_string(std::max(request.output_tokens, 0)) +
                 ","
                 "cache_read_tokens=cache_read_tokens+" +
                 std::to_string(std::max(request.cache_read_tokens, 0)) +
                 ","
                 "cache_creation_tokens=cache_creation_tokens+" +
                 std::to_string(cache_creation) +
                 ","
                 "tokens=tokens+" +
                 std::to_string(total_tokens) +
                 ","
                 "usd=usd+" +
                 usd_buf +
                 ","
                 "first_token_latency_sum=first_token_latency_sum+" +
                 std::to_string(ftl));
    t.commit();
}

} // namespace revlm
