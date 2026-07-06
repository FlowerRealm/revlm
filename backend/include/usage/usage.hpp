#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "store/mysql.hpp"

namespace revlm
{

constexpr std::string_view usage_state_pending = "pending";
constexpr std::string_view usage_state_committed = "committed";
constexpr std::string_view usage_state_failed = "failed";

struct UsageBeginInput {
    std::string request_id;
    long long user_id = 0;
    long long token_id = 0;
    std::optional<std::string> model;
    std::optional<std::string> requested_service_tier;
    std::optional<std::string> service_tier;
    std::optional<std::string> service_tier_downgrade_reason;
    std::string committed_usd = "0.000000";
};

struct UsageCommitInput {
    long long usage_event_id = 0;
    std::optional<long long> channel_id;
    std::optional<std::string> requested_service_tier;
    std::optional<std::string> service_tier;
    std::optional<std::string> service_tier_downgrade_reason;
    std::optional<long long> input_tokens;
    std::optional<long long> cache_read_input_tokens;
    std::optional<long long> cache_creation_input_tokens;
    std::optional<long long> cache_creation_1h_input_tokens;
    std::optional<long long> output_tokens;
    std::string committed_usd = "0.000000";
    std::string price_multiplier = "1.000000";
    std::string price_multiplier_group = "1.000000";
    std::string price_multiplier_payment = "1.000000";
    std::optional<std::string> price_multiplier_group_name;
};

struct UsageFinalizeInput {
    long long usage_event_id = 0;
    std::optional<std::string> endpoint;
    std::optional<std::string> method;
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    std::optional<std::string> error_class;
    std::optional<std::string> error_message;
    std::optional<std::string> forwarded_model;
    std::optional<std::string> upstream_response_model;
    std::optional<long long> channel_id;
    bool is_stream = false;
    long long request_bytes = 0;
    long long response_bytes = 0;
};

struct UsageEventRecord {
    long long id = 0;
    std::string request_id;
    long long user_id = 0;
    long long token_id = 0;
    std::string state;
    std::optional<std::string> model;
    std::optional<std::string> endpoint;
    std::optional<std::string> method;
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    std::optional<std::string> error_class;
    std::optional<std::string> error_message;
    std::optional<std::string> forwarded_model;
    std::optional<std::string> upstream_response_model;
    std::optional<long long> channel_id;
    std::optional<std::string> requested_service_tier;
    std::optional<std::string> service_tier;
    std::optional<std::string> service_tier_downgrade_reason;
    std::optional<long long> input_tokens;
    std::optional<long long> cache_read_input_tokens;
    std::optional<long long> cache_creation_input_tokens;
    std::optional<long long> cache_creation_1h_input_tokens;
    std::optional<long long> output_tokens;
    std::string committed_usd;
    std::string price_multiplier;
    std::string price_multiplier_group;
    std::string price_multiplier_payment;
    std::optional<std::string> price_multiplier_group_name;
    bool is_stream = false;
    long long request_bytes = 0;
    long long response_bytes = 0;
};

struct UsageExpireResult {
    long long expired = 0;
    std::vector<long long> ids;
};

using UsageCommitAction = std::function<void()>;

struct UsageEvent {
    long long id = 0;
    std::string time;
    std::string request_id;
    std::optional<std::string> endpoint;
    std::optional<std::string> method;
    long long user_id = 0;
    long long token_id = 0;
    std::optional<long long> channel_id;
    std::string state;
    std::optional<std::string> model;
    std::optional<std::string> forwarded_model;
    std::optional<std::string> upstream_response_model;
    std::optional<std::string> requested_service_tier;
    std::optional<std::string> service_tier;
    std::optional<std::string> service_tier_downgrade_reason;
    std::optional<long long> input_tokens;
    std::optional<long long> cache_read_input_tokens;
    std::optional<long long> cache_creation_input_tokens;
    std::optional<long long> cache_creation_1h_input_tokens;
    std::optional<long long> output_tokens;
    std::string committed_usd;
    std::optional<std::string> price_multiplier;
    std::optional<std::string> price_multiplier_group;
    std::optional<std::string> price_multiplier_payment;
    std::optional<std::string> price_multiplier_group_name;
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    std::optional<std::string> error_class;
    std::optional<std::string> error_message;
    bool is_stream = false;
    long long request_bytes = 0;
    long long response_bytes = 0;
    std::string created_at;
    std::string updated_at;
};

struct UsageEventWithUser {
    UsageEvent event;
    std::string user_email;
};

struct UsageTokenStats {
    long long requests = 0;
    long long tokens = 0;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_input_tokens = 0;
    long long cache_creation_input_tokens = 0;
    long long first_token_samples = 0;
    double cache_ratio = 0.0;
    double avg_first_token_ms = 0.0;
    double output_tokens_per_sec = 0.0;
    std::string committed_usd = "0";
};

struct UsageTimeSeriesPoint {
    std::string bucket;
    long long requests = 0;
    long long tokens = 0;
    double committed_usd = 0.0;
    double cache_ratio = 0.0;
    double avg_first_token_latency = 0.0;
    double tokens_per_second = 0.0;
};

struct UsageTopUser {
    long long user_id = 0;
    std::string email;
    std::string role;
    int status = 0;
    std::string committed_usd = "0";
};

struct UsageUserSuggest {
    long long id = 0;
    std::string email;
    std::string username;
};

struct UsageChannelSuggest {
    long long id = 0;
    std::string name;
    std::string type;
};

struct UsageEventsIndexFlags {
    bool user = false;
    bool key = false;
    bool channel = false;
    bool model = false;
};

struct UsageEventsFilters {
    std::optional<long long> user_id;
    std::optional<long long> channel_id;
    std::optional<std::string> model_exact;
    std::string user;
    std::string key;
    std::string channel;
    std::string model;
};

bool usage_model_mismatch(const UsageEvent &event);
std::string normalize_usage_service_tier(std::string_view raw);
std::string normalize_usage_service_tier_downgrade_reason(std::string_view raw);
std::optional<std::string> normalize_usage_service_tier(const std::optional<std::string> &value);
std::optional<std::string> normalize_usage_service_tier_downgrade_reason(const std::optional<std::string> &value);
std::optional<UsageEventRecord> row_to_usage_event_record(const MysqlResultRow &row);

class UsageStore {
public:
    explicit UsageStore(MysqlConnection &conn);

    long long begin_usage(const UsageBeginInput &input);
    bool commit_usage(const UsageCommitInput &input);
    bool finalize_usage(const UsageFinalizeInput &input);
    bool fail_usage(long long usage_event_id, const std::optional<std::string> &error_class,
                    const std::optional<std::string> &error_message);
    bool abort_usage(long long usage_event_id);
    UsageExpireResult expire_pending_usage(std::string_view before_utc);
    std::optional<UsageEventRecord> get_usage_event_by_id(long long usage_event_id);
    std::optional<UsageEventRecord> get_usage_event_by_request_id(std::string_view request_id);
    bool commit_non_stream_before_response(const UsageCommitInput &commit, const UsageFinalizeInput &finalize,
                                           UsageCommitAction action);

    std::optional<UsageEvent> get_usage_event(long long event_id);

    UsageTokenStats get_usage_token_stats_by_user_range(long long user_id, std::string_view since_utc,
                                                        std::string_view until_utc);
    UsageTokenStats get_usage_token_stats_by_token_range(long long token_id, std::string_view since_utc,
                                                         std::string_view until_utc);
    UsageTokenStats get_global_usage_stats_range(std::string_view since_utc, std::string_view until_utc);

    std::vector<UsageEvent> list_usage_events_by_token(long long token_id, int limit,
                                                       const std::optional<long long> &before_id);
    std::vector<UsageEvent> list_usage_events_by_token_range(long long token_id, std::string_view since_utc,
                                                             std::string_view until_utc, int limit,
                                                             const std::optional<long long> &before_id,
                                                             const std::optional<long long> &after_id);
    std::vector<UsageEvent> list_usage_events_by_user_filtered(long long user_id, int limit,
                                                               const std::optional<long long> &before_id,
                                                               const std::optional<long long> &after_id,
                                                               const UsageEventsIndexFlags &index_flags,
                                                               const UsageEventsFilters &filters);
    std::vector<UsageEvent> list_usage_events_by_user_range_filtered(long long user_id, std::string_view since_utc,
                                                                     std::string_view until_utc, int limit,
                                                                     const std::optional<long long> &before_id,
                                                                     const std::optional<long long> &after_id,
                                                                     const UsageEventsIndexFlags &index_flags,
                                                                     const UsageEventsFilters &filters);
    std::vector<UsageEventWithUser> list_usage_events_with_user_range_filtered(
        std::string_view since_utc, std::string_view until_utc, int limit, const std::optional<long long> &before_id,
        const std::optional<long long> &after_id, const UsageEventsIndexFlags &index_flags,
        const UsageEventsFilters &filters);

    std::vector<UsageTimeSeriesPoint> get_user_usage_time_series_range(long long user_id, std::string_view since_utc,
                                                                       std::string_view until_utc,
                                                                       std::string_view granularity);
    std::vector<UsageTimeSeriesPoint> get_token_usage_time_series_range(long long token_id, std::string_view since_utc,
                                                                        std::string_view until_utc,
                                                                        std::string_view granularity);
    std::vector<UsageTimeSeriesPoint> get_global_usage_time_series_range(std::string_view since_utc,
                                                                         std::string_view until_utc,
                                                                         std::string_view granularity);

    std::vector<UsageTopUser> list_usage_top_users(std::string_view since_utc, std::string_view until_utc, int limit);
    std::vector<UsageUserSuggest> suggest_users(std::string_view q, int limit);
    std::vector<UsageChannelSuggest> suggest_usage_channels(std::string_view since_utc, std::string_view until_utc,
                                                            std::string_view q, int limit);
    std::vector<std::string> suggest_usage_models(std::string_view since_utc, std::string_view until_utc,
                                                  std::string_view q, int limit);

private:
    MysqlConnection &conn_;
};

} // namespace revlm
