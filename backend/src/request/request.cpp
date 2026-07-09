#include "request/request.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>

#include "util/strings.hpp"

namespace revlm
{
namespace
{

std::string format_multiplier(double value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", value);
    return std::string{ buf };
}

} // namespace

Request::Request() = default;

double Request::solve_price() const
{
    return (model.input_price * input_tokens / 1000000 + model.output_price * output_tokens / 1000000 +
            model.cache_read_price * cache_read_tokens / 1000000 +
            model.cache_creation_1h_price * cache_creation_1h_tokens / 1000000 +
            model.cache_creation_5m_price * cache_creation_5m_tokens / 1000000) *
           tier_multiplier * channel_multiplier;
}

std::string request_timestamp_now()
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

bool Request::commit_usage_event(MysqlConnection &conn, std::string_view finished_at) const
{
    if (id <= 0 || user_id <= 0 || token_id <= 0) {
        return false;
    }
    const long long existing =
        std::stoll(conn.query_one("SELECT COUNT(*) FROM usage_events WHERE id=" + std::to_string(id)).value_or("0"));
    if (existing > 0) {
        return true;
    }

    const std::string occurred_at = !trim_ascii(time).empty()        ? trim_ascii(time) :
                                    !trim_ascii(finished_at).empty() ? trim_ascii(finished_at) :
                                                                       request_timestamp_now();
    const std::string status = "committed";
    const std::optional<std::string> model_name = model.name.empty() ? std::nullopt :
                                                                       std::optional<std::string>{ model.name };
    const std::optional<std::string> service_tier_value =
        service_tier.empty() ? std::nullopt : std::optional<std::string>{ service_tier };
    const std::optional<std::string> endpoint_value = endpoint.empty() ? std::nullopt :
                                                                         std::optional<std::string>{ endpoint };
    const std::optional<std::string> method_value = method.empty() ? std::nullopt :
                                                                     std::optional<std::string>{ method };
    const std::optional<std::string> error_class_value =
        error_class.empty() ? std::nullopt : std::optional<std::string>{ error_class };
    const std::optional<std::string> error_message_value =
        error_message.empty() ? std::nullopt : std::optional<std::string>{ error_message };

    DbTransaction tr(conn);
    conn.exec("INSERT INTO usage_events("
              "id,time,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
              "error_class,error_message,user_id,token_id,channel_id,status,model,service_tier,"
              "input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
              "output_tokens,tier_multiplier,channel_multiplier,is_stream"
              ") VALUES(" +
              std::to_string(id) + "," + conn.quote(occurred_at) + "," + sql_nullable(conn, endpoint_value) + "," +
              sql_nullable(conn, method_value) + "," + std::to_string(std::max(status_code, 0)) + "," +
              std::to_string(std::max(latency_ms, 0)) + "," + std::to_string(std::max(first_token_latency_ms, 0)) +
              "," + sql_nullable(conn, error_class_value) + "," + sql_nullable(conn, error_message_value) + "," +
              std::to_string(user_id) + "," + std::to_string(token_id) + "," + std::to_string(channel_id) + "," +
              conn.quote(status) + "," + sql_nullable(conn, model_name) + "," + sql_nullable(conn, service_tier_value) +
              "," + std::to_string(std::max(input_tokens, 0)) + "," + std::to_string(std::max(cache_read_tokens, 0)) +
              "," + std::to_string(std::max(cache_creation_5m_tokens, 0)) + "," +
              std::to_string(std::max(cache_creation_1h_tokens, 0)) + "," + std::to_string(std::max(output_tokens, 0)) +
              "," + format_multiplier(tier_multiplier) + "," + format_multiplier(channel_multiplier) + "," +
              std::to_string(is_stream ? 1 : 0) + ")");
    if (conn.affected_rows() == 0) {
        return false;
    }
    tr.commit();
    return true;
}

} // namespace revlm
