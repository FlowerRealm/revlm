#include "usage/usage_queries.hpp"

#include "models/models.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

constexpr int default_limit = 50;
constexpr int max_limit = 200;

std::string json_escape_local(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out += "\\u00";
                constexpr char hex[] = "0123456789abcdef";
                out.push_back(hex[(static_cast<unsigned char>(ch) >> 4) & 0xf]);
                out.push_back(hex[static_cast<unsigned char>(ch) & 0xf]);
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    return out;
}

std::optional<long long> opt_i64(const std::optional<std::string> &value)
{
    if (!value.has_value()) {
        return std::nullopt;
    }
    try {
        return std::stoll(trim_ascii(*value));
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

long long i64_or_zero(const std::optional<std::string> &value)
{
    return opt_i64(value).value_or(0);
}

int int_or_zero(const std::optional<std::string> &value)
{
    return static_cast<int>(i64_or_zero(value));
}

bool bool_from_i64(const std::optional<std::string> &value)
{
    return i64_or_zero(value) != 0;
}

std::optional<std::string> nullable_trimmed(const std::optional<std::string> &value)
{
    if (!value.has_value()) {
        return std::nullopt;
    }
    std::string out = trim_ascii(*value);
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

std::string decimal_to_string(double value)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(6);
    out << value;
    return out.str();
}

double parse_decimal(std::string_view raw)
{
    try {
        return std::stod(std::string{ raw });
    } catch (const std::exception &) {
        return 0.0;
    }
}

std::string json_string_or_null(const std::optional<std::string> &value)
{
    if (!value.has_value()) {
        return "null";
    }
    return "\"" + json_escape_local(*value) + "\"";
}

std::string json_string(std::string_view value)
{
    return "\"" + json_escape_local(value) + "\"";
}

std::string mysql_to_iso_utc(std::string_view value)
{
    if (value.empty()) {
        return {};
    }
    std::string out{ value };
    if (out.size() == 19) {
        for (char &ch : out) {
            if (ch == ' ') {
                ch = 'T';
                break;
            }
        }
        out += "Z";
    }
    return out;
}

std::string price_string(double price)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6f", price);
    return std::string{ buffer };
}

std::string status_json_label(std::string_view status)
{
    if (status == "committed" || status == "1" || status == "true" || status == "TRUE") {
        return "committed";
    }
    return std::string{ status };
}

UsageEventRow row_to_usage_event(const MysqlResultRow &row)
{
    // SELECT order (joined list query):
    // 0 id, 1 time, 2 endpoint, 3 method, 4 status_code, 5 latency_ms, 6 first_token_latency_ms,
    // 7 error_class, 8 error_message, 9 user_id, 10 email, 11 token_id, 12 channel_id, 13 channel name,
    // 14 status, 15 model, 16 service_tier, 17 input_tokens, 18 cache_read_tokens,
    // 19 cache_creation_5m_tokens, 20 cache_creation_1h_tokens, 21 output_tokens,
    // 22 tier_multiplier, 23 channel_multiplier, 24 is_stream
    UsageEventRow out;
    out.id = i64_or_zero(row.size() > 0 ? row[0] : std::nullopt);
    out.time = mysql_to_iso_utc(row.size() > 1 ? row[1].value_or("") : "");
    out.endpoint = nullable_trimmed(row.size() > 2 ? row[2] : std::nullopt);
    out.method = nullable_trimmed(row.size() > 3 ? row[3] : std::nullopt);
    out.status_code = int_or_zero(row.size() > 4 ? row[4] : std::nullopt);
    out.latency_ms = int_or_zero(row.size() > 5 ? row[5] : std::nullopt);
    out.first_token_latency_ms = int_or_zero(row.size() > 6 ? row[6] : std::nullopt);
    out.error_class = nullable_trimmed(row.size() > 7 ? row[7] : std::nullopt);
    out.error_message = nullable_trimmed(row.size() > 8 ? row[8] : std::nullopt);
    out.user_id = i64_or_zero(row.size() > 9 ? row[9] : std::nullopt);
    out.user_email = row.size() > 10 ? row[10].value_or("") : "";
    out.token_id = i64_or_zero(row.size() > 11 ? row[11] : std::nullopt);
    out.channel_id = i64_or_zero(row.size() > 12 ? row[12] : std::nullopt);
    out.upstream_channel_name = nullable_trimmed(row.size() > 13 ? row[13] : std::nullopt);
    out.status = status_json_label(row.size() > 14 ? row[14].value_or("") : "");
    out.model = nullable_trimmed(row.size() > 15 ? row[15] : std::nullopt);
    out.service_tier = nullable_trimmed(row.size() > 16 ? row[16] : std::nullopt);
    out.input_tokens = opt_i64(row.size() > 17 ? row[17] : std::nullopt);
    out.cache_read_tokens = opt_i64(row.size() > 18 ? row[18] : std::nullopt);
    out.cache_creation_5m_tokens = opt_i64(row.size() > 19 ? row[19] : std::nullopt);
    out.cache_creation_1h_tokens = opt_i64(row.size() > 20 ? row[20] : std::nullopt);
    out.output_tokens = opt_i64(row.size() > 21 ? row[21] : std::nullopt);
    {
        const std::string tier_raw = trim_ascii(row.size() > 22 ? row[22].value_or("") : "");
        out.tier_multiplier = tier_raw.empty() ? 1.0 : parse_decimal(tier_raw);
        const std::string channel_raw = trim_ascii(row.size() > 23 ? row[23].value_or("") : "");
        out.channel_multiplier = channel_raw.empty() ? 1.0 : parse_decimal(channel_raw);
    }
    out.is_stream = bool_from_i64(row.size() > 24 ? row[24] : std::nullopt);

    Request req{ Model{}, 0, 0, 0, 0, 0 };
    req.id = out.id;
    req.time = out.time;
    req.endpoint = out.endpoint.value_or("");
    req.method = out.method.value_or("");
    req.status_code = out.status_code;
    req.latency_ms = out.latency_ms;
    req.first_token_latency_ms = out.first_token_latency_ms;
    req.error_class = out.error_class.value_or("");
    req.error_message = out.error_message.value_or("");
    req.user_id = out.user_id;
    req.token_id = out.token_id;
    req.channel_id = out.channel_id;
    req.statue = out.status == "committed";
    req.model.name = out.model.value_or("");
    req.service_tier = out.service_tier.value_or("");
    req.input_tokens = static_cast<int>(out.input_tokens.value_or(0));
    req.cache_read_tokens = static_cast<int>(out.cache_read_tokens.value_or(0));
    req.cache_creation_5m_tokens = static_cast<int>(out.cache_creation_5m_tokens.value_or(0));
    req.cache_creation_1h_tokens = static_cast<int>(out.cache_creation_1h_tokens.value_or(0));
    req.output_tokens = static_cast<int>(out.output_tokens.value_or(0));
    req.tier_multiplier = out.tier_multiplier;
    req.channel_multiplier = out.channel_multiplier;
    req.is_stream = out.is_stream;
    hydrate_request_model(req);
    out.committed_usd = decimal_to_string(req.solve_price());
    return out;
}

std::string usage_event_select_sql()
{
    return std::string{
        "SELECT "
        "e.id,e.time,e.endpoint,e.method,e.status_code,e.latency_ms,e.first_token_latency_ms,"
        "e.error_class,e.error_message,e.user_id,u.email,e.token_id,e.channel_id,c.name,"
        "e.status,e.model,e.service_tier,e.input_tokens,e.cache_read_tokens,"
        "e.cache_creation_5m_tokens,e.cache_creation_1h_tokens,e.output_tokens,"
        "e.tier_multiplier,e.channel_multiplier,e.is_stream "
        "FROM usage_events e "
        "JOIN users u ON u.id=e.user_id "
        "LEFT JOIN channels c ON c.id=e.channel_id "
    };
}

std::string state_label(std::string_view status)
{
    if (status == "pending")
        return "处理中";
    if (status == "committed")
        return "已结算";
    if (status == "void")
        return "已作废";
    if (status == "expired")
        return "已过期";
    return std::string{ status };
}

std::string state_badge_class(std::string_view status)
{
    if (status == "pending")
        return "bg-warning-subtle text-warning border border-warning-subtle";
    if (status == "committed")
        return "bg-success-subtle text-success border border-success-subtle";
    return "bg-secondary-subtle text-secondary border border-secondary-subtle";
}

std::string event_error_text(const UsageEventRow &event)
{
    const std::string cls = event.error_class.value_or("");
    const std::string msg = event.error_message.value_or("");
    if (!cls.empty() && !msg.empty()) {
        return cls + " (" + msg + ")";
    }
    return !cls.empty() ? cls : msg;
}

std::string sql_like_contains(MysqlConnection &conn, std::string_view raw)
{
    std::string value = trim_ascii(raw);
    return conn.quote("%" + value + "%");
}

void append_filter(std::vector<std::string> &conditions, std::string condition)
{
    if (!condition.empty()) {
        conditions.push_back(std::move(condition));
    }
}

std::string join_conditions(const std::vector<std::string> &conditions)
{
    if (conditions.empty()) {
        return {};
    }
    std::string out = " WHERE ";
    for (size_t i = 0; i < conditions.size(); ++i) {
        if (i > 0) {
            out += " AND ";
        }
        out += conditions[i];
    }
    return out;
}

std::string event_detail_select_sql()
{
    return std::string{
        "SELECT "
        "id,time,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
        "error_class,error_message,user_id,token_id,channel_id,status,model,service_tier,"
        "input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
        "output_tokens,tier_multiplier,channel_multiplier,is_stream "
        "FROM usage_events "
    };
}

std::optional<UsageEventDetail> row_to_usage_event_detail(const MysqlResultRow &row)
{
    if (row.size() < 23 || !row[0].has_value()) {
        return std::nullopt;
    }
    Request req = row_to_request(row);
    hydrate_request_model(req);

    UsageEventDetail detail;
    detail.event_id = req.id;

    UsageEventPricingBreakdown pricing;
    const std::string model_id = trim_ascii(req.model.name);
    const std::vector<Model> &models = ModelManager::instance().models();
    const auto builtin_it = std::ranges::find(models, model_id, &Model::name);
    const bool found = builtin_it != models.end();
    pricing.model_public_id = model_id.empty() ? std::nullopt : std::optional<std::string>{ model_id };
    pricing.model_found = found;
    pricing.owned_by = found ? std::optional<std::string>{ builtin_it->owned_by } : std::optional<std::string>{ "openai" };
    pricing.service_tier = req.service_tier.empty() ? std::nullopt : std::optional<std::string>{ req.service_tier };

    pricing.input_tokens_total = req.input_tokens;
    pricing.input_tokens_cache_read = req.cache_read_tokens;
    pricing.input_tokens_cache_creation_5m = req.cache_creation_5m_tokens;
    pricing.input_tokens_cache_creation_1h = req.cache_creation_1h_tokens;
    pricing.input_tokens_cache_creation = req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
    pricing.output_tokens_total = req.output_tokens;
    pricing.input_tokens_billable =
        std::max(0, req.input_tokens - req.cache_read_tokens - req.cache_creation_5m_tokens -
                        req.cache_creation_1h_tokens);

    pricing.input_usd_per_1m = found ? price_string(builtin_it->input_price) : "0.000000";
    pricing.output_usd_per_1m = found ? price_string(builtin_it->output_price) : "0.000000";
    pricing.cache_read_usd_per_1m = found ? price_string(builtin_it->cache_read_price) : "0.000000";
    pricing.cache_creation_5m_usd_per_1m = found ? price_string(builtin_it->cache_creation_5m_price) : "0.000000";
    pricing.cache_creation_1h_usd_per_1m = found ? price_string(builtin_it->cache_creation_1h_price) : "0.000000";

    const double input_rate = parse_decimal(pricing.input_usd_per_1m) / 1000000.0;
    const double output_rate = parse_decimal(pricing.output_usd_per_1m) / 1000000.0;
    const double cache_read_rate = parse_decimal(pricing.cache_read_usd_per_1m) / 1000000.0;
    const double cache_create_5m_rate = parse_decimal(pricing.cache_creation_5m_usd_per_1m) / 1000000.0;
    const double cache_create_1h_rate = parse_decimal(pricing.cache_creation_1h_usd_per_1m) / 1000000.0;

    const double input_cost = static_cast<double>(pricing.input_tokens_billable) * input_rate;
    const double output_cost = static_cast<double>(pricing.output_tokens_total) * output_rate;
    const double cache_read_cost = static_cast<double>(pricing.input_tokens_cache_read) * cache_read_rate;
    const double cache_create_5m_cost =
        static_cast<double>(pricing.input_tokens_cache_creation_5m) * cache_create_5m_rate;
    const double cache_create_1h_cost =
        static_cast<double>(pricing.input_tokens_cache_creation_1h) * cache_create_1h_rate;
    const double cache_create_total_cost = cache_create_5m_cost + cache_create_1h_cost;
    const double base_cost = input_cost + output_cost + cache_read_cost + cache_create_total_cost;

    pricing.input_cost_usd = decimal_to_string(input_cost);
    pricing.output_cost_usd = decimal_to_string(output_cost);
    pricing.cache_read_cost_usd = decimal_to_string(cache_read_cost);
    pricing.cache_creation_cost_usd = decimal_to_string(cache_create_total_cost);
    pricing.cache_creation_5m_cost_usd = decimal_to_string(cache_create_5m_cost);
    pricing.cache_creation_1h_cost_usd = decimal_to_string(cache_create_1h_cost);
    pricing.base_cost_usd = decimal_to_string(base_cost);
    pricing.tier_multiplier = decimal_to_string(req.tier_multiplier);
    pricing.channel_multiplier = decimal_to_string(req.channel_multiplier);
    pricing.final_cost_usd = decimal_to_string(req.solve_price());

    detail.pricing_breakdown = pricing;
    return detail;
}

std::string bool_json(bool value)
{
    return value ? "true" : "false";
}

std::string token_json_number_or_null(const std::optional<long long> &value)
{
    if (!value.has_value())
        return "null";
    return std::to_string(*value);
}

} // namespace

UsageQueryStore::UsageQueryStore(MysqlConnection &conn)
    : conn_(conn)
{
}

UsageEventsPage UsageQueryStore::list_user_usage_events(long long user_id, const UsageQueryFilters &filters)
{
    UsageQueryFilters with_user = filters;
    with_user.user_id = user_id;
    return list_usage_events(with_user, "e.user_id=" + std::to_string(user_id));
}

UsageEventsPage UsageQueryStore::list_admin_usage_events(const UsageQueryFilters &filters)
{
    return list_usage_events(filters, "");
}

UsageEventsPage UsageQueryStore::list_usage_events(const UsageQueryFilters &filters, const std::string &owner_sql)
{
    UsageQueryFilters normalized = filters;
    if (normalized.limit <= 0) {
        normalized.limit = default_limit;
    }
    if (normalized.limit > max_limit) {
        normalized.limit = max_limit;
    }
    if (normalized.before_id.has_value() && normalized.after_id.has_value()) {
        throw std::invalid_argument("before_id and after_id cannot be combined");
    }

    std::vector<std::string> conditions;
    append_filter(conditions, owner_sql);
    if (normalized.token_id.has_value()) {
        append_filter(conditions, "e.token_id=" + std::to_string(*normalized.token_id));
    }
    if (normalized.user_id.has_value()) {
        append_filter(conditions, "e.user_id=" + std::to_string(*normalized.user_id));
    }
    if (normalized.channel_id.has_value()) {
        append_filter(conditions, "e.channel_id=" + std::to_string(*normalized.channel_id));
    }
    if (normalized.model.has_value()) {
        append_filter(conditions, "e.model=" + conn_.quote(*normalized.model));
    }
    if (normalized.start.has_value()) {
        append_filter(conditions, "e.time >= " + conn_.quote(*normalized.start));
    }
    if (normalized.end_exclusive.has_value()) {
        append_filter(conditions, "e.time < " + conn_.quote(*normalized.end_exclusive));
    } else if (normalized.end.has_value()) {
        append_filter(conditions, "e.time <= " + conn_.quote(*normalized.end));
    }
    if (normalized.q_user.has_value() && !trim_ascii(*normalized.q_user).empty()) {
        const std::string pattern = sql_like_contains(conn_, *normalized.q_user);
        append_filter(conditions, "(u.email LIKE " + pattern + " OR u.username LIKE " + pattern + ")");
    }
    if (normalized.q_key.has_value() && !trim_ascii(*normalized.q_key).empty()) {
        const std::string pattern = sql_like_contains(conn_, *normalized.q_key);
        std::string token_filter = "e.token_id IN (SELECT id FROM user_tokens WHERE name LIKE " + pattern;
        if (normalized.user_id.has_value()) {
            token_filter += " AND user_id=" + std::to_string(*normalized.user_id);
        }
        token_filter += ")";
        append_filter(conditions, std::move(token_filter));
    }
    if (normalized.q_channel.has_value() && !trim_ascii(*normalized.q_channel).empty()) {
        const std::string pattern = sql_like_contains(conn_, *normalized.q_channel);
        append_filter(conditions,
                      "EXISTS (SELECT 1 FROM channels uc WHERE uc.id=e.channel_id AND uc.name LIKE " + pattern + ")");
    }
    if (normalized.q_model.has_value() && !trim_ascii(*normalized.q_model).empty()) {
        append_filter(conditions, "e.model LIKE " + sql_like_contains(conn_, *normalized.q_model));
    }
    if (normalized.before_id.has_value()) {
        append_filter(conditions, "e.id < " + std::to_string(*normalized.before_id));
    }
    if (normalized.after_id.has_value()) {
        append_filter(conditions, "e.id > " + std::to_string(*normalized.after_id));
    }

    std::string order = normalized.after_id.has_value() ? " ORDER BY e.id ASC" : " ORDER BY e.id DESC";
    std::string sql = usage_event_select_sql() + join_conditions(conditions) + order + " LIMIT " +
                      std::to_string(normalized.limit + 1);
    const auto rows = conn_.query_rows(sql);

    UsageEventsPage page;
    page.cursor_active = normalized.before_id.has_value() || normalized.after_id.has_value();

    std::vector<UsageEventRow> events;
    events.reserve(std::min(static_cast<size_t>(normalized.limit), rows.size()));
    for (const auto &row : rows) {
        if (events.size() == static_cast<size_t>(normalized.limit)) {
            break;
        }
        events.push_back(row_to_usage_event(row));
    }
    const bool has_extra = rows.size() > static_cast<size_t>(normalized.limit);

    if (normalized.after_id.has_value()) {
        std::reverse(events.begin(), events.end());
        if (!events.empty()) {
            page.prev_after_id = events.front().id;
        }
        if (has_extra && !events.empty()) {
            page.next_before_id = events.back().id;
        }
    } else {
        if (has_extra && !events.empty()) {
            page.next_before_id = events.back().id;
        }
        if (normalized.before_id.has_value() && !events.empty()) {
            page.prev_after_id = events.front().id;
        }
    }
    page.events = std::move(events);
    return page;
}

std::optional<UsageEventDetail> UsageQueryStore::get_user_usage_event_detail(long long user_id, long long event_id,
                                                                             std::optional<long long> token_id)
{
    std::string where = "id=" + std::to_string(event_id) + " AND user_id=" + std::to_string(user_id);
    if (token_id.has_value()) {
        where += " AND token_id=" + std::to_string(*token_id);
    }
    return get_usage_event_detail(where);
}

std::optional<UsageEventDetail> UsageQueryStore::get_admin_usage_event_detail(long long event_id)
{
    return get_usage_event_detail("id=" + std::to_string(event_id));
}

std::optional<UsageEventDetail> UsageQueryStore::get_usage_event_detail(const std::string &where_sql)
{
    const auto rows = conn_.query_rows(event_detail_select_sql() + " WHERE " + where_sql + " LIMIT 1");
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_usage_event_detail(rows.front());
}

std::string usage_event_to_user_json(const UsageEventRow &event)
{
    const long long cache_creation =
        event.cache_creation_5m_tokens.value_or(0) + event.cache_creation_1h_tokens.value_or(0);
    std::string out = "{";
    out += "\"id\":" + std::to_string(event.id);
    out += ",\"time\":" + json_string(event.time);
    out += ",\"request_id\":" + json_string(std::to_string(event.id));
    out += ",\"endpoint\":" + json_string_or_null(event.endpoint);
    out += ",\"method\":" + json_string_or_null(event.method);
    out += ",\"token_id\":" + std::to_string(event.token_id);
    out += ",\"channel_id\":" + (event.channel_id > 0 ? std::to_string(event.channel_id) : "null");
    out += ",\"status\":" + json_string(event.status);
    out += ",\"model\":" + json_string_or_null(event.model);
    out += ",\"service_tier\":" + json_string_or_null(event.service_tier);
    out += ",\"input_tokens\":" + token_json_number_or_null(event.input_tokens);
    out += ",\"cache_read_tokens\":" + token_json_number_or_null(event.cache_read_tokens);
    out += ",\"cache_creation_5m_tokens\":" + token_json_number_or_null(event.cache_creation_5m_tokens);
    out += ",\"cache_creation_1h_tokens\":" + token_json_number_or_null(event.cache_creation_1h_tokens);
    out += ",\"cache_creation_tokens\":" + std::to_string(cache_creation);
    out += ",\"output_tokens\":" + token_json_number_or_null(event.output_tokens);
    out += ",\"committed_usd\":" + json_string(event.committed_usd);
    out += ",\"status_code\":" + std::to_string(event.status_code);
    out += ",\"latency_ms\":" + std::to_string(event.latency_ms);
    out += ",\"error_class\":" + json_string_or_null(event.error_class);
    out += ",\"error_message\":" + json_string_or_null(event.error_message);
    out += ",\"is_stream\":" + bool_json(event.is_stream);
    out += "}";
    return out;
}

std::string usage_event_to_admin_json(const UsageEventRow &event)
{
    const long long cached_tokens = event.cache_read_tokens.value_or(0) +
                                    event.cache_creation_5m_tokens.value_or(0) +
                                    event.cache_creation_1h_tokens.value_or(0);
    const std::string tps = (event.output_tokens.value_or(0) > 0 && event.latency_ms > 0) ?
                                decimal_to_string(static_cast<double>(event.output_tokens.value_or(0)) * 1000.0 /
                                                  static_cast<double>(event.latency_ms)) :
                                "-";
    std::string out = "{";
    out += "\"id\":" + std::to_string(event.id);
    out += ",\"time\":" + json_string(event.time);
    out += ",\"user_id\":" + std::to_string(event.user_id);
    out += ",\"user_email\":" + json_string(event.user_email);
    out += ",\"endpoint\":" + json_string(event.endpoint.value_or(""));
    out += ",\"method\":" + json_string(event.method.value_or(""));
    out += ",\"model\":" + json_string(event.model.value_or(""));
    out += ",\"status_code\":" + json_string(std::to_string(event.status_code));
    out += ",\"latency_ms\":" + json_string(std::to_string(event.latency_ms));
    out += ",\"first_token_latency_ms\":" + json_string(std::to_string(event.first_token_latency_ms));
    out += ",\"tokens_per_second\":" + json_string(tps);
    out += ",\"input_tokens\":" + json_string(std::to_string(event.input_tokens.value_or(0)));
    out += ",\"output_tokens\":" + json_string(std::to_string(event.output_tokens.value_or(0)));
    out += ",\"cached_tokens\":" + json_string(cached_tokens > 0 ? std::to_string(cached_tokens) : "-");
    out += ",\"cost_usd\":" + json_string(event.committed_usd);
    out += ",\"committed_usd\":" + json_string(event.committed_usd);
    out += ",\"status\":" + json_string(event.status);
    out += ",\"state_label\":" + json_string(state_label(event.status));
    out += ",\"state_badge_class\":" + json_string(state_badge_class(event.status));
    out += ",\"service_tier\":" + json_string_or_null(event.service_tier);
    out += ",\"is_stream\":" + bool_json(event.is_stream);
    out += ",\"channel_id\":" + json_string(event.channel_id > 0 ? std::to_string(event.channel_id) : "-");
    out += ",\"upstream_channel_name\":" + json_string(event.upstream_channel_name.value_or(""));
    out += ",\"request_id\":" + json_string(std::to_string(event.id));
    out += ",\"error\":" + json_string(event_error_text(event));
    out += ",\"error_class\":" + json_string(event.error_class.value_or(""));
    out += ",\"error_message\":" + json_string(event.error_message.value_or(""));
    out += "}";
    return out;
}

std::string usage_events_page_to_user_json(const UsageEventsPage &page)
{
    std::string out = "{\"events\":[";
    for (size_t i = 0; i < page.events.size(); ++i) {
        if (i > 0)
            out += ",";
        out += usage_event_to_user_json(page.events[i]);
    }
    out += "]";
    if (page.next_before_id.has_value()) {
        out += ",\"next_before_id\":" + std::to_string(*page.next_before_id);
    } else {
        out += ",\"next_before_id\":null";
    }
    out += "}";
    return out;
}

std::string usage_events_page_to_admin_json(const UsageEventsPage &page)
{
    std::string out = "{\"events\":[";
    for (size_t i = 0; i < page.events.size(); ++i) {
        if (i > 0)
            out += ",";
        out += usage_event_to_admin_json(page.events[i]);
    }
    out += "]";
    out += ",\"next_before_id\":";
    out += page.next_before_id.has_value() ? std::to_string(*page.next_before_id) : "null";
    out += ",\"prev_after_id\":";
    out += page.prev_after_id.has_value() ? std::to_string(*page.prev_after_id) : "null";
    out += ",\"cursor_active\":";
    out += bool_json(page.cursor_active);
    out += "}";
    return out;
}

std::string usage_event_detail_to_json(const UsageEventDetail &detail)
{
    std::string out = "{\"event_id\":" + std::to_string(detail.event_id);
    if (detail.pricing_breakdown.has_value()) {
        const auto &p = *detail.pricing_breakdown;
        out += ",\"pricing_breakdown\":{";
        out += "\"model_public_id\":" + json_string_or_null(p.model_public_id);
        out += ",\"model_found\":" + bool_json(p.model_found);
        out += ",\"owned_by\":" + json_string_or_null(p.owned_by);
        out += ",\"service_tier\":" + json_string_or_null(p.service_tier);
        out += ",\"pricing_kind\":" + json_string(p.pricing_kind);
        out += ",\"input_tokens_total\":" + std::to_string(p.input_tokens_total);
        out += ",\"input_tokens_cache_read\":" + std::to_string(p.input_tokens_cache_read);
        out += ",\"input_tokens_cache_creation\":" + std::to_string(p.input_tokens_cache_creation);
        out += ",\"input_tokens_cache_creation_5m\":" + std::to_string(p.input_tokens_cache_creation_5m);
        out += ",\"input_tokens_cache_creation_1h\":" + std::to_string(p.input_tokens_cache_creation_1h);
        out += ",\"input_tokens_billable\":" + std::to_string(p.input_tokens_billable);
        out += ",\"output_tokens_total\":" + std::to_string(p.output_tokens_total);
        out += ",\"input_usd_per_1m\":" + json_string(p.input_usd_per_1m);
        out += ",\"output_usd_per_1m\":" + json_string(p.output_usd_per_1m);
        out += ",\"cache_read_usd_per_1m\":" + json_string(p.cache_read_usd_per_1m);
        out += ",\"cache_creation_5m_usd_per_1m\":" + json_string(p.cache_creation_5m_usd_per_1m);
        out += ",\"cache_creation_1h_usd_per_1m\":" + json_string(p.cache_creation_1h_usd_per_1m);
        out += ",\"input_cost_usd\":" + json_string(p.input_cost_usd);
        out += ",\"output_cost_usd\":" + json_string(p.output_cost_usd);
        out += ",\"cache_read_cost_usd\":" + json_string(p.cache_read_cost_usd);
        out += ",\"cache_creation_cost_usd\":" + json_string(p.cache_creation_cost_usd);
        out += ",\"cache_creation_5m_cost_usd\":" + json_string(p.cache_creation_5m_cost_usd);
        out += ",\"cache_creation_1h_cost_usd\":" + json_string(p.cache_creation_1h_cost_usd);
        out += ",\"base_cost_usd\":" + json_string(p.base_cost_usd);
        out += ",\"tier_multiplier\":" + json_string(p.tier_multiplier);
        out += ",\"channel_multiplier\":" + json_string(p.channel_multiplier);
        out += ",\"final_cost_usd\":" + json_string(p.final_cost_usd);
        out += "}";
    }
    out += "}";
    return out;
}

namespace
{

bool parse_i64(std::string_view raw, long long &out)
{
    const std::string trimmed = trim_ascii(raw);
    if (trimmed.empty()) {
        return false;
    }
    bool negative = false;
    size_t pos = 0;
    if (trimmed[0] == '-') {
        negative = true;
        pos = 1;
    }
    if (pos >= trimmed.size()) {
        return false;
    }
    long long value = 0;
    for (; pos < trimmed.size(); ++pos) {
        const char ch = trimmed[pos];
        if (ch < '0' || ch > '9') {
            return false;
        }
        const int digit = ch - '0';
        if (value > (std::numeric_limits<long long>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = negative ? -value : value;
    return true;
}

long long parse_request_i64_or_zero(const std::optional<std::string> &raw)
{
    if (!raw.has_value()) {
        return 0;
    }
    long long value = 0;
    return parse_i64(*raw, value) ? value : 0;
}

int parse_request_int_or_zero(const std::optional<std::string> &raw)
{
    const long long value = parse_request_i64_or_zero(raw);
    if (value < std::numeric_limits<int>::min()) {
        return std::numeric_limits<int>::min();
    }
    if (value > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(value);
}

double parse_request_double_or_zero(const std::optional<std::string> &raw)
{
    if (!raw.has_value()) {
        return 0.0;
    }
    try {
        return std::stod(trim_ascii(*raw));
    } catch (const std::exception &) {
        return 0.0;
    }
}

std::string optional_request_string(const std::optional<std::string> &raw)
{
    return raw.has_value() ? trim_ascii(*raw) : std::string{};
}

bool parse_request_boolish(std::string_view raw)
{
    const std::string trimmed = trim_ascii(raw);
    return trimmed == "1" || trimmed == "true" || trimmed == "TRUE";
}

} // namespace

std::string normalize_usage_service_tier(std::string_view raw)
{
    std::string tier = lowercase_ascii(trim_ascii(raw));
    if (tier == "fast") {
        tier = "priority";
    }
    return tier;
}

std::optional<std::string> normalize_usage_service_tier(const std::optional<std::string> &value)
{
    constexpr size_t service_tier_max_len = 32;
    if (!value.has_value()) {
        return std::nullopt;
    }
    std::string tier = lowercase_ascii(trim_ascii(*value));
    if (tier.empty()) {
        return std::nullopt;
    }
    if (tier == "fast") {
        tier = "priority";
    }
    if (tier.size() > service_tier_max_len) {
        tier.resize(service_tier_max_len);
    }
    return tier;
}

Request row_to_request(const MysqlResultRow &row)
{
    Request req{ Model{}, 0, 0, 0, 0, 0 };
    req.user_id = 0;
    req.channel_id = 0;
    req.statue = false;
    if (row.size() < 23) {
        return req;
    }

    req.id = parse_request_i64_or_zero(row[0]);
    req.time = row[1].value_or("");
    req.endpoint = optional_request_string(row[2]);
    req.method = optional_request_string(row[3]);
    req.status_code = parse_request_int_or_zero(row[4]);
    req.latency_ms = parse_request_int_or_zero(row[5]);
    req.first_token_latency_ms = parse_request_int_or_zero(row[6]);
    req.error_class = optional_request_string(row[7]);
    req.error_message = optional_request_string(row[8]);
    req.user_id = parse_request_i64_or_zero(row[9]);
    req.token_id = parse_request_i64_or_zero(row[10]);
    req.channel_id = parse_request_i64_or_zero(row[11]);

    const std::string status = optional_request_string(row[12]);
    req.statue = parse_request_boolish(status) || status == "committed";

    req.model.name = optional_request_string(row[13]);
    req.service_tier = optional_request_string(row[14]);
    req.input_tokens = parse_request_int_or_zero(row[15]);
    req.cache_read_tokens = parse_request_int_or_zero(row[16]);
    req.cache_creation_5m_tokens = parse_request_int_or_zero(row[17]);
    req.cache_creation_1h_tokens = parse_request_int_or_zero(row[18]);
    req.output_tokens = parse_request_int_or_zero(row[19]);
    req.tier_multiplier = optional_request_string(row[20]).empty() ? 1.0 : parse_request_double_or_zero(row[20]);
    req.channel_multiplier = optional_request_string(row[21]).empty() ? 1.0 : parse_request_double_or_zero(row[21]);
    req.is_stream = parse_request_boolish(row[22].value_or("0"));
    return req;
}

void hydrate_request_model(Request &req)
{
    const std::string &name = req.model.name;
    if (name.empty()) {
        return;
    }
    const std::vector<Model> &models = ModelManager::instance().models();
    const auto it = std::ranges::find(models, name, &Model::name);
    if (it != models.end()) {
        req.model = *it;
    }
}

} // namespace revlm
