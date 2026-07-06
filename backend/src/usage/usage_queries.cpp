#include "usage/usage_queries.hpp"

#include "models/models.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
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
    long long out = 0;
    const auto [ptr, ec] = std::from_chars(value->data(), value->data() + value->size(), out);
    if (ec != std::errc{} || ptr != value->data() + value->size()) {
        return std::nullopt;
    }
    return out;
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

std::string json_i64_or_null(const std::optional<long long> &value)
{
    if (!value.has_value()) {
        return "null";
    }
    return std::to_string(*value);
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

UsageEventRow row_to_usage_event(const MysqlResultRow &row)
{
    UsageEventRow out;
    out.id = i64_or_zero(row.size() > 0 ? row[0] : std::nullopt);
    out.time = mysql_to_iso_utc(row.size() > 1 ? row[1].value_or("") : "");
    out.request_id = row.size() > 2 ? row[2].value_or("") : "";
    out.endpoint = nullable_trimmed(row.size() > 3 ? row[3] : std::nullopt);
    out.method = nullable_trimmed(row.size() > 4 ? row[4] : std::nullopt);
    out.user_id = i64_or_zero(row.size() > 5 ? row[5] : std::nullopt);
    out.user_email = row.size() > 6 ? row[6].value_or("") : "";
    out.token_id = i64_or_zero(row.size() > 7 ? row[7] : std::nullopt);
    out.channel_id = opt_i64(row.size() > 8 ? row[8] : std::nullopt);
    out.upstream_channel_name = nullable_trimmed(row.size() > 9 ? row[9] : std::nullopt);
    out.upstream_account_id = nullable_trimmed(row.size() > 10 ? row[10] : std::nullopt);
    out.state = row.size() > 11 ? row[11].value_or("") : "";
    out.model = nullable_trimmed(row.size() > 12 ? row[12] : std::nullopt);
    out.requested_service_tier = nullable_trimmed(row.size() > 13 ? row[13] : std::nullopt);
    out.service_tier = nullable_trimmed(row.size() > 14 ? row[14] : std::nullopt);
    out.service_tier_downgrade_reason = nullable_trimmed(row.size() > 15 ? row[15] : std::nullopt);
    out.input_tokens = opt_i64(row.size() > 16 ? row[16] : std::nullopt);
    out.cache_read_input_tokens = opt_i64(row.size() > 17 ? row[17] : std::nullopt);
    out.cache_creation_input_tokens = opt_i64(row.size() > 18 ? row[18] : std::nullopt);
    out.output_tokens = opt_i64(row.size() > 19 ? row[19] : std::nullopt);
    out.committed_usd = row.size() > 20 ? row[20].value_or("0.000000") : "0.000000";
    out.status_code = int_or_zero(row.size() > 21 ? row[21] : std::nullopt);
    out.latency_ms = int_or_zero(row.size() > 22 ? row[22] : std::nullopt);
    out.first_token_latency_ms = int_or_zero(row.size() > 23 ? row[23] : std::nullopt);
    out.error_class = nullable_trimmed(row.size() > 24 ? row[24] : std::nullopt);
    out.error_message = nullable_trimmed(row.size() > 25 ? row[25] : std::nullopt);
    out.is_stream = bool_from_i64(row.size() > 26 ? row[26] : std::nullopt);
    out.request_bytes = i64_or_zero(row.size() > 27 ? row[27] : std::nullopt);
    out.response_bytes = i64_or_zero(row.size() > 28 ? row[28] : std::nullopt);
    out.model_mismatch = bool_from_i64(row.size() > 29 ? row[29] : std::nullopt);
    out.created_at = mysql_to_iso_utc(row.size() > 30 ? row[30].value_or("") : "");
    out.updated_at = mysql_to_iso_utc(row.size() > 31 ? row[31].value_or("") : "");
    return out;
}

std::string usage_event_select_sql()
{
    return std::string{
        "SELECT "
        "e.id,e.time,e.request_id,e.endpoint,e.method,"
        "e.user_id,u.email,e.token_id,"
        "e.channel_id,c.name,"
        "NULL AS account_id,"
        "e.state,e.model,e.requested_service_tier,e.service_tier,e.service_tier_downgrade_reason,"
        "e.input_tokens,e.cache_read_input_tokens,e.cache_creation_input_tokens,e.output_tokens,"
        "e.committed_usd,e.status_code,e.latency_ms,e.first_token_latency_ms,e.error_class,e.error_message,"
        "e.is_stream,e.request_bytes,e.response_bytes,"
        "CASE WHEN COALESCE(NULLIF(TRIM(e.forwarded_model),''),COALESCE(NULLIF(TRIM(e.model),''),'')) "
        "<> COALESCE(NULLIF(TRIM(e.upstream_response_model),''),COALESCE(NULLIF(TRIM(e.forwarded_model),''),COALESCE(NULLIF(TRIM(e.model),''),''))) "
        "THEN 1 ELSE 0 END AS model_mismatch,"
        "e.created_at,e.updated_at "
        "FROM usage_events e "
        "JOIN users u ON u.id=e.user_id "
        "LEFT JOIN channels c ON c.id=e.channel_id "
    };
}

std::string state_label(std::string_view state)
{
    if (state == "pending")
        return "处理中";
    if (state == "committed")
        return "已结算";
    if (state == "void")
        return "已作废";
    if (state == "expired")
        return "已过期";
    return std::string{ state };
}

std::string state_badge_class(std::string_view state)
{
    if (state == "pending")
        return "bg-warning-subtle text-warning border border-warning-subtle";
    if (state == "committed")
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
        "e.id,e.model,e.forwarded_model,e.upstream_response_model,"
        "e.requested_service_tier,e.service_tier,e.service_tier_downgrade_reason,"
        "e.input_tokens,e.cache_read_input_tokens,e.cache_creation_input_tokens,e.cache_creation_1h_input_tokens,e.output_tokens,"
        "e.committed_usd,e.price_multiplier,e.price_multiplier_group,e.price_multiplier_payment,e.price_multiplier_group_name "
        "FROM usage_events e "
    };
}

std::optional<UsageEventDetail> row_to_usage_event_detail(const MysqlResultRow &row)
{
    if (row.size() < 17 || !row[0].has_value()) {
        return std::nullopt;
    }
    UsageEventDetail detail;
    detail.event_id = i64_or_zero(row[0]);

    UsageEventModelCheck model_check;
    model_check.forwarded_model = nullable_trimmed(row[2]);
    model_check.upstream_response_model = nullable_trimmed(row[3]);
    const std::string lhs = trim_ascii(model_check.forwarded_model.value_or(nullable_trimmed(row[1]).value_or("")));
    const std::string rhs = trim_ascii(model_check.upstream_response_model.value_or(lhs));
    model_check.mismatch = !lhs.empty() && !rhs.empty() && lhs != rhs;
    if (model_check.forwarded_model.has_value() || model_check.upstream_response_model.has_value() ||
        model_check.mismatch) {
        detail.model_check = model_check;
    }

    UsageEventPricingBreakdown pricing;
    const std::string model_id = trim_ascii(nullable_trimmed(row[1]).value_or(""));
    const std::vector<Model> &models = ModelManager::instance().models();
    const auto builtin_it = std::ranges::find(models, model_id, &Model::name);
    const bool builtin_found = builtin_it != models.end();
    pricing.model_public_id = model_id.empty() ? std::nullopt : std::optional<std::string>{ model_id };
    pricing.model_found = builtin_found;
    pricing.owned_by = builtin_found ? std::optional<std::string>{ builtin_it->owned_by } :
                                       std::optional<std::string>{ "openai" };
    pricing.requested_service_tier = nullable_trimmed(row[4]);
    pricing.service_tier = nullable_trimmed(row[5]);
    pricing.service_tier_downgrade_reason = nullable_trimmed(row[6]);
    pricing.service_tier_downgraded = pricing.requested_service_tier.has_value() && pricing.service_tier.has_value() &&
                                      lowercase_ascii(*pricing.requested_service_tier) !=
                                          lowercase_ascii(*pricing.service_tier);
    pricing.effective_service_tier = pricing.service_tier.has_value() ? pricing.service_tier :
                                                                        pricing.requested_service_tier;
    pricing.input_tokens_total = i64_or_zero(row[7]);
    pricing.input_tokens_cache_read = i64_or_zero(row[8]);
    pricing.input_tokens_cache_creation = i64_or_zero(row[9]);
    pricing.input_tokens_cache_creation_1h = i64_or_zero(row[10]);
    pricing.input_tokens_cache_creation_5m =
        std::max<long long>(0, pricing.input_tokens_cache_creation - pricing.input_tokens_cache_creation_1h);
    pricing.output_tokens_total = i64_or_zero(row[11]);
    pricing.input_tokens_billable = std::max<long long>(
        0, pricing.input_tokens_total - pricing.input_tokens_cache_read - pricing.input_tokens_cache_creation);

    pricing.final_cost_usd = row[12].value_or("0.000000");
    pricing.effective_multiplier = row[13].value_or("1.000000");
    pricing.group_multiplier = row[14].value_or("1.000000");
    pricing.payment_multiplier = row[15].value_or("1.000000");
    pricing.group_name = nullable_trimmed(row[16]).value_or("default");
    auto price_string = [](double price) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.6f", price);
        return std::string{ buffer };
    };
    pricing.input_usd_per_1m = builtin_found ? price_string(builtin_it->input_price) : "0.000000";
    pricing.output_usd_per_1m = builtin_found ? price_string(builtin_it->output_price) : "0.000000";
    pricing.cache_read_input_usd_per_1m = builtin_found ? price_string(builtin_it->cache_read_price) : "0.000000";
    pricing.cache_creation_input_usd_per_1m = builtin_found ? price_string(builtin_it->cache_creation_5m_price) :
                                                              "0.000000";
    pricing.cache_creation_1h_input_usd_per_1m = builtin_found ? price_string(builtin_it->cache_creation_1h_price) :
                                                                 "0.000000";

    const double input_rate = parse_decimal(pricing.input_usd_per_1m) / 1000000.0;
    const double output_rate = parse_decimal(pricing.output_usd_per_1m) / 1000000.0;
    const double cache_read_rate = parse_decimal(pricing.cache_read_input_usd_per_1m) / 1000000.0;
    const double cache_create_rate = parse_decimal(pricing.cache_creation_input_usd_per_1m) / 1000000.0;
    const double cache_create_1h_rate = parse_decimal(pricing.cache_creation_1h_input_usd_per_1m) / 1000000.0;

    const double input_cost = static_cast<double>(pricing.input_tokens_billable) * input_rate;
    const double output_cost = static_cast<double>(pricing.output_tokens_total) * output_rate;
    const double cache_read_cost = static_cast<double>(pricing.input_tokens_cache_read) * cache_read_rate;
    const double cache_create_5m_cost = static_cast<double>(pricing.input_tokens_cache_creation_5m) * cache_create_rate;
    const double cache_create_1h_cost =
        static_cast<double>(pricing.input_tokens_cache_creation_1h) * cache_create_1h_rate;
    const double cache_create_total_cost = cache_create_5m_cost + cache_create_1h_cost;
    const double base_cost = input_cost + output_cost + cache_read_cost + cache_create_total_cost;

    pricing.input_cost_usd = decimal_to_string(input_cost);
    pricing.output_cost_usd = decimal_to_string(output_cost);
    pricing.cache_read_input_cost_usd = decimal_to_string(cache_read_cost);
    pricing.cache_creation_input_cost_usd = decimal_to_string(cache_create_total_cost);
    pricing.cache_creation_5m_input_cost_usd = decimal_to_string(cache_create_5m_cost);
    pricing.cache_creation_1h_input_cost_usd = decimal_to_string(cache_create_1h_cost);
    pricing.base_cost_usd = decimal_to_string(base_cost);
    pricing.high_context_applied = false;
    pricing.high_context_threshold_tokens = 0;
    pricing.high_context_trigger_input_tokens = pricing.input_tokens_total;

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
    return list_usage_events(filters, "e.user_id=" + std::to_string(user_id));
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
    std::string where = "e.id=" + std::to_string(event_id) + " AND e.user_id=" + std::to_string(user_id);
    if (token_id.has_value()) {
        where += " AND e.token_id=" + std::to_string(*token_id);
    }
    return get_usage_event_detail(where);
}

std::optional<UsageEventDetail> UsageQueryStore::get_admin_usage_event_detail(long long event_id)
{
    return get_usage_event_detail("e.id=" + std::to_string(event_id));
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
    std::string out = "{";
    out += "\"id\":" + std::to_string(event.id);
    out += ",\"time\":" + json_string(event.time);
    out += ",\"request_id\":" + json_string(event.request_id);
    out += ",\"endpoint\":" + json_string_or_null(event.endpoint);
    out += ",\"method\":" + json_string_or_null(event.method);
    out += ",\"token_id\":" + std::to_string(event.token_id);
    out += ",\"channel_id\":" + json_i64_or_null(event.channel_id);
    out += ",\"state\":" + json_string(event.state);
    out += ",\"model\":" + json_string_or_null(event.model);
    out += ",\"requested_service_tier\":" + json_string_or_null(event.requested_service_tier);
    out += ",\"service_tier\":" + json_string_or_null(event.service_tier);
    out += ",\"service_tier_downgraded\":" +
           bool_json(event.model_mismatch ?
                         false :
                         (event.requested_service_tier.has_value() && event.service_tier.has_value() &&
                          lowercase_ascii(*event.requested_service_tier) != lowercase_ascii(*event.service_tier)));
    out += ",\"service_tier_downgrade_reason\":" + json_string_or_null(event.service_tier_downgrade_reason);
    out += ",\"input_tokens\":" + token_json_number_or_null(event.input_tokens);
    out += ",\"cache_read_input_tokens\":" + token_json_number_or_null(event.cache_read_input_tokens);
    out += ",\"output_tokens\":" + token_json_number_or_null(event.output_tokens);
    out += ",\"cache_creation_input_tokens\":" + token_json_number_or_null(event.cache_creation_input_tokens);
    out += ",\"committed_usd\":" + json_string(event.committed_usd);
    out += ",\"status_code\":" + std::to_string(event.status_code);
    out += ",\"latency_ms\":" + std::to_string(event.latency_ms);
    out += ",\"error_class\":" + json_string_or_null(event.error_class);
    out += ",\"error_message\":" + json_string_or_null(event.error_message);
    out += ",\"is_stream\":" + bool_json(event.is_stream);
    out += ",\"request_bytes\":" + std::to_string(event.request_bytes);
    out += ",\"response_bytes\":" + std::to_string(event.response_bytes);
    out += ",\"model_mismatch\":" + bool_json(event.model_mismatch);
    out += ",\"created_at\":" + json_string(event.created_at);
    out += ",\"updated_at\":" + json_string(event.updated_at);
    out += "}";
    return out;
}

std::string usage_event_to_admin_json(const UsageEventRow &event)
{
    const long long cached_tokens =
        event.cache_read_input_tokens.value_or(0) + event.cache_creation_input_tokens.value_or(0);
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
    out += ",\"account\":" + json_string(event.upstream_account_id.value_or("-"));
    out += ",\"status_code\":" + json_string(std::to_string(event.status_code));
    out += ",\"latency_ms\":" + json_string(std::to_string(event.latency_ms));
    out += ",\"first_token_latency_ms\":" + json_string(std::to_string(event.first_token_latency_ms));
    out += ",\"tokens_per_second\":" + json_string(tps);
    out += ",\"input_tokens\":" + json_string(std::to_string(event.input_tokens.value_or(0)));
    out += ",\"output_tokens\":" + json_string(std::to_string(event.output_tokens.value_or(0)));
    out += ",\"cached_tokens\":" + json_string(cached_tokens > 0 ? std::to_string(cached_tokens) : "-");
    out += ",\"request_bytes\":" + json_string(std::to_string(event.request_bytes));
    out += ",\"response_bytes\":" + json_string(std::to_string(event.response_bytes));
    out += ",\"cost_usd\":" + json_string(event.committed_usd);
    out += ",\"state_label\":" + json_string(state_label(event.state));
    out += ",\"state_badge_class\":" + json_string(state_badge_class(event.state));
    out += ",\"requested_service_tier\":" + json_string_or_null(event.requested_service_tier);
    out += ",\"service_tier\":" + json_string_or_null(event.service_tier);
    out += ",\"service_tier_downgraded\":" +
           bool_json(event.requested_service_tier.has_value() && event.service_tier.has_value() &&
                     lowercase_ascii(*event.requested_service_tier) != lowercase_ascii(*event.service_tier));
    out += ",\"service_tier_downgrade_reason\":" + json_string_or_null(event.service_tier_downgrade_reason);
    out += ",\"is_stream\":" + bool_json(event.is_stream);
    out += ",\"channel_id\":" + json_string(event.channel_id.has_value() ? std::to_string(*event.channel_id) : "-");
    out += ",\"upstream_channel_name\":" + json_string(event.upstream_channel_name.value_or(""));
    out += ",\"request_id\":" + json_string(event.request_id);
    out += ",\"error\":" + json_string(event_error_text(event));
    out += ",\"error_class\":" + json_string(event.error_class.value_or(""));
    out += ",\"error_message\":" + json_string(event.error_message.value_or(""));
    out += ",\"model_mismatch\":" + bool_json(event.model_mismatch);
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
        out += ",\"requested_service_tier\":" + json_string_or_null(p.requested_service_tier);
        out += ",\"service_tier\":" + json_string_or_null(p.service_tier);
        out += ",\"service_tier_downgraded\":" + bool_json(p.service_tier_downgraded);
        out += ",\"service_tier_downgrade_reason\":" + json_string_or_null(p.service_tier_downgrade_reason);
        out += ",\"pricing_kind\":" + json_string(p.pricing_kind);
        out += ",\"high_context_applied\":" + bool_json(p.high_context_applied);
        out += ",\"high_context_threshold_tokens\":" + std::to_string(p.high_context_threshold_tokens);
        out += ",\"high_context_trigger_input_tokens\":" + std::to_string(p.high_context_trigger_input_tokens);
        out += ",\"effective_service_tier\":" + json_string_or_null(p.effective_service_tier);
        out += ",\"input_tokens_total\":" + std::to_string(p.input_tokens_total);
        out += ",\"input_tokens_cache_read\":" + std::to_string(p.input_tokens_cache_read);
        out += ",\"input_tokens_cache_creation\":" + std::to_string(p.input_tokens_cache_creation);
        out += ",\"input_tokens_cache_creation_5m\":" + std::to_string(p.input_tokens_cache_creation_5m);
        out += ",\"input_tokens_cache_creation_1h\":" + std::to_string(p.input_tokens_cache_creation_1h);
        out += ",\"input_tokens_billable\":" + std::to_string(p.input_tokens_billable);
        out += ",\"output_tokens_total\":" + std::to_string(p.output_tokens_total);
        out += ",\"input_usd_per_1m\":" + json_string(p.input_usd_per_1m);
        out += ",\"output_usd_per_1m\":" + json_string(p.output_usd_per_1m);
        out += ",\"cache_read_input_usd_per_1m\":" + json_string(p.cache_read_input_usd_per_1m);
        out += ",\"cache_creation_input_usd_per_1m\":" + json_string(p.cache_creation_input_usd_per_1m);
        out += ",\"cache_creation_1h_input_usd_per_1m\":" + json_string(p.cache_creation_1h_input_usd_per_1m);
        out += ",\"input_cost_usd\":" + json_string(p.input_cost_usd);
        out += ",\"output_cost_usd\":" + json_string(p.output_cost_usd);
        out += ",\"cache_read_input_cost_usd\":" + json_string(p.cache_read_input_cost_usd);
        out += ",\"cache_creation_input_cost_usd\":" + json_string(p.cache_creation_input_cost_usd);
        out += ",\"cache_creation_5m_input_cost_usd\":" + json_string(p.cache_creation_5m_input_cost_usd);
        out += ",\"cache_creation_1h_input_cost_usd\":" + json_string(p.cache_creation_1h_input_cost_usd);
        out += ",\"base_cost_usd\":" + json_string(p.base_cost_usd);
        out += ",\"payment_multiplier\":" + json_string(p.payment_multiplier);
        out += ",\"group_name\":" + json_string(p.group_name);
        out += ",\"group_multiplier\":" + json_string(p.group_multiplier);
        out += ",\"effective_multiplier\":" + json_string(p.effective_multiplier);
        out += ",\"final_cost_usd\":" + json_string(p.final_cost_usd);
        out += "}";
    }
    if (detail.model_check.has_value()) {
        const auto &m = *detail.model_check;
        out += ",\"model_check\":{";
        out += "\"forwarded_model\":" + json_string_or_null(m.forwarded_model);
        out += ",\"upstream_response_model\":" + json_string_or_null(m.upstream_response_model);
        out += ",\"mismatch\":" + bool_json(m.mismatch);
        out += "}";
    }
    out += "}";
    return out;
}

} // namespace revlm
