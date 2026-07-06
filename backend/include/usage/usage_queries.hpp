#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    std::string request_id;
    std::optional<std::string> endpoint;
    std::optional<std::string> method;
    long long user_id = 0;
    std::string user_email;
    long long token_id = 0;
    std::optional<long long> channel_id;
    std::optional<std::string> upstream_channel_name;
    std::optional<std::string> upstream_account_id;
    std::string state;
    std::optional<std::string> model;
    std::optional<std::string> requested_service_tier;
    std::optional<std::string> service_tier;
    std::optional<std::string> service_tier_downgrade_reason;
    std::optional<long long> input_tokens;
    std::optional<long long> cache_read_input_tokens;
    std::optional<long long> cache_creation_input_tokens;
    std::optional<long long> output_tokens;
    std::string committed_usd;
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    std::optional<std::string> error_class;
    std::optional<std::string> error_message;
    bool is_stream = false;
    long long request_bytes = 0;
    long long response_bytes = 0;
    bool model_mismatch = false;
    std::string created_at;
    std::string updated_at;
};

struct UsageEventsPage {
    std::vector<UsageEventRow> events;
    std::optional<long long> next_before_id;
    std::optional<long long> prev_after_id;
    bool cursor_active = false;
};

struct UsageEventModelCheck {
    std::optional<std::string> forwarded_model;
    std::optional<std::string> upstream_response_model;
    bool mismatch = false;
};

struct UsageEventPricingBreakdown {
    std::optional<std::string> model_public_id;
    bool model_found = false;
    std::optional<std::string> owned_by;
    std::optional<std::string> requested_service_tier;
    std::optional<std::string> service_tier;
    bool service_tier_downgraded = false;
    std::optional<std::string> service_tier_downgrade_reason;
    std::string pricing_kind = "base";
    bool high_context_applied = false;
    long long high_context_threshold_tokens = 0;
    long long high_context_trigger_input_tokens = 0;
    std::optional<std::string> effective_service_tier;

    long long input_tokens_total = 0;
    long long input_tokens_cache_read = 0;
    long long input_tokens_cache_creation = 0;
    long long input_tokens_cache_creation_5m = 0;
    long long input_tokens_cache_creation_1h = 0;
    long long input_tokens_billable = 0;
    long long output_tokens_total = 0;

    std::string input_usd_per_1m = "0.000000";
    std::string output_usd_per_1m = "0.000000";
    std::string cache_read_input_usd_per_1m = "0.000000";
    std::string cache_creation_input_usd_per_1m = "0.000000";
    std::string cache_creation_1h_input_usd_per_1m = "0.000000";

    std::string input_cost_usd = "0.000000";
    std::string output_cost_usd = "0.000000";
    std::string cache_read_input_cost_usd = "0.000000";
    std::string cache_creation_input_cost_usd = "0.000000";
    std::string cache_creation_5m_input_cost_usd = "0.000000";
    std::string cache_creation_1h_input_cost_usd = "0.000000";
    std::string base_cost_usd = "0.000000";

    std::string payment_multiplier = "1.000000";
    std::string group_name = "default";
    std::string group_multiplier = "1.000000";
    std::string effective_multiplier = "1.000000";

    std::string final_cost_usd = "0.000000";
};

struct UsageEventDetail {
    long long event_id = 0;
    std::optional<UsageEventPricingBreakdown> pricing_breakdown;
    std::optional<UsageEventModelCheck> model_check;
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

} // namespace revlm
