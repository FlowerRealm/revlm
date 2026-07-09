#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "request/request.hpp"
#include "store/mysql.hpp"

namespace revlm
{

struct UsageQueryFilters {
    std::optional<std::string> start;
    std::optional<std::string> end;
    std::optional<std::string> end_exclusive;
    std::optional<long long> token_id;
    std::optional<long long> user_id;
    std::optional<long long> channel_id;
    std::optional<long long> before_id;
    std::optional<long long> after_id;
    std::optional<std::string> model;
    std::optional<std::string> q_key;
    std::optional<std::string> q_user;
    std::optional<std::string> q_channel;
    std::optional<std::string> q_model;
    int limit = 50;
};

struct UsageEventRow {
    long long id = 0;
    std::string time;
    std::optional<std::string> endpoint;
    std::optional<std::string> method;
    long long user_id = 0;
    std::string user_email;
    long long token_id = 0;
    long long channel_id = 0;
    std::optional<std::string> upstream_channel_name;
    std::string status;
    std::optional<std::string> model;
    std::optional<std::string> service_tier;
    std::optional<long long> input_tokens;
    std::optional<long long> cache_read_tokens;
    std::optional<long long> cache_creation_5m_tokens;
    std::optional<long long> cache_creation_1h_tokens;
    std::optional<long long> output_tokens;
    double tier_multiplier = 1.0;
    double channel_multiplier = 1.0;
    std::string committed_usd = "0.000000";
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    std::optional<std::string> error_class;
    std::optional<std::string> error_message;
    bool is_stream = false;
};

struct UsageEventsPage {
    std::vector<UsageEventRow> events;
    std::optional<long long> next_before_id;
    std::optional<long long> prev_after_id;
    bool cursor_active = false;
};

struct UsageEventPricingBreakdown {
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

struct UsageEventDetail {
    long long event_id = 0;
    std::optional<UsageEventPricingBreakdown> pricing_breakdown;
};

class UsageQueryStore {
public:
    explicit UsageQueryStore(MysqlConnection &conn);

    UsageEventsPage list_user_usage_events(long long user_id, const UsageQueryFilters &filters);
    UsageEventsPage list_admin_usage_events(const UsageQueryFilters &filters);

    std::optional<UsageEventDetail> get_user_usage_event_detail(long long user_id, long long event_id,
                                                                std::optional<long long> token_id);
    std::optional<UsageEventDetail> get_admin_usage_event_detail(long long event_id);

private:
    UsageEventsPage list_usage_events(const UsageQueryFilters &filters, const std::string &owner_sql);
    std::optional<UsageEventDetail> get_usage_event_detail(const std::string &where_sql);

    MysqlConnection &conn_;
};

std::string usage_event_to_user_json(const UsageEventRow &event);
std::string usage_event_to_admin_json(const UsageEventRow &event);
std::string usage_events_page_to_user_json(const UsageEventsPage &page);
std::string usage_events_page_to_admin_json(const UsageEventsPage &page);
std::string usage_event_detail_to_json(const UsageEventDetail &detail);

std::string normalize_usage_service_tier(std::string_view raw);
std::optional<std::string> normalize_usage_service_tier(const std::optional<std::string> &value);

Request row_to_request(const MysqlResultRow &row);
void hydrate_request_model(Request &req);

} // namespace revlm
