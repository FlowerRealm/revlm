#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "store/mysql.hpp"

namespace revlm
{

constexpr std::string_view usage_commit_job_state_streaming = "streaming";
constexpr std::string_view usage_commit_job_state_ready = "ready";
constexpr std::string_view usage_commit_job_state_processing = "processing";
constexpr std::string_view usage_commit_job_state_done = "done";
constexpr std::string_view usage_commit_job_state_aborted = "aborted";
constexpr std::string_view usage_commit_job_state_dead_letter = "dead_letter";

struct UsageFinalizePayload {
    std::optional<std::string> endpoint;
    std::optional<std::string> method;
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    std::optional<std::string> error_class;
    std::optional<std::string> error_message;
    std::optional<long long> channel_id;
    bool is_stream = false;
    long long request_bytes = 0;
    long long response_bytes = 0;
};

struct UsageCommitPricingSnapshot {
    std::string owned_by = "openai";
    std::string input_usd_per_1m = "0.000000";
    std::string output_usd_per_1m = "0.000000";
    std::string cache_read_input_usd_per_1m = "0.000000";
    std::string cache_creation_input_usd_per_1m = "0.000000";
    std::string cache_creation_1h_input_usd_per_1m = "0.000000";
};

struct UsageCommitPayload {
    std::string request_id;
    long long user_id = 0;
    long long token_id = 0;
    std::optional<std::string> occurred_at;
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
    std::string committed_usd = "0.000000";
    std::string price_multiplier = "1.000000";
    std::string price_multiplier_group = "1.000000";
    std::string price_multiplier_payment = "1.000000";
    std::optional<std::string> price_multiplier_group_name;
    UsageCommitPricingSnapshot pricing;
    std::optional<long long> prepared_usage_event_id;
    bool direct_commit = false;
    bool balance_debited = false;
    bool retryable = false;
    UsageFinalizePayload finalize;
};

struct UsageCommitJob {
    long long id = 0;
    std::string request_id;
    long long user_id = 0;
    long long token_id = 0;
    std::string state;
    std::optional<std::string> lease_token;
    std::optional<std::string> lease_until;
    int attempts = 0;
    UsageCommitPayload payload;
    std::string created_at;
    std::string updated_at;
};

struct UsageCommitJobInput {
    std::string request_id;
    long long user_id = 0;
    long long token_id = 0;
    UsageCommitPayload payload;
};

struct UsageCommitFinalizeInput {
    long long job_id = 0;
    std::string from_state = std::string{ usage_commit_job_state_streaming };
    std::string to_state = std::string{ usage_commit_job_state_ready };
    UsageCommitPayload payload;
    std::optional<std::string> finished_at;
};

struct UsageCommitClaimInput {
    int limit = 64;
    std::string lease_token;
    std::optional<std::string> lease_until;
};

struct UsageCommitCompletionResult {
    int completed = 0;
    int dead_lettered = 0;
    int requeued = 0;
};

class UsageCommitJobStore {
public:
    explicit UsageCommitJobStore(MysqlConnection &conn);

    long long create_usage_commit_job(const UsageCommitJobInput &input);
    std::vector<long long> create_usage_commit_jobs_fast(const std::vector<UsageCommitJobInput> &inputs);
    bool finalize_usage_commit_job(const UsageCommitFinalizeInput &input);
    std::vector<UsageCommitJob> claim_ready_usage_commit_jobs(const UsageCommitClaimInput &input);
    bool extend_processing_usage_commit_jobs_lease(std::string_view lease_token, std::string_view lease_until);
    bool update_processing_usage_commit_job_state(long long job_id, std::string_view lease_token,
                                                  std::string_view to_state);
    long long abort_stale_streaming_usage_commit_jobs(std::string_view before_time);
    bool complete_usage_commit_job(long long job_id, std::string_view lease_token, std::string_view finished_at);
    UsageCommitCompletionResult complete_usage_commit_jobs(const std::vector<UsageCommitJob> &jobs,
                                                           std::string_view lease_token, std::string_view finished_at);
    bool commit_usage_payload_direct(const UsageCommitJobInput &input, std::string_view finished_at);
    std::optional<UsageCommitJob> get_usage_commit_job_by_id(long long job_id);
    std::optional<UsageCommitJob> get_usage_commit_job_by_request_id(std::string_view request_id);
    long long count_usage_events_by_request_id(std::string_view request_id);
    std::optional<std::string> usage_event_state_by_request_id(std::string_view request_id);

private:
    bool commit_claimed_job(long long job_id, std::string_view lease_token, std::string_view finished_at,
                            UsageCommitCompletionResult *stats);
    bool write_usage_event(const UsageCommitPayload &payload, std::string_view finished_at);
    MysqlConnection &conn_;
};

struct UsageCommitWorkerMetrics {
    int worker_count = 0;
    int claim_size = 0;
    unsigned long long claimed_total = 0;
    unsigned long long completed_total = 0;
    unsigned long long dead_letter_total = 0;
    unsigned long long requeued_total = 0;
};

class UsageCommitWorker {
public:
    UsageCommitWorker(UsageCommitJobStore &store, const Config &config);

    UsageCommitWorkerMetrics drain_once();

private:
    std::string make_lease_token() const;

    UsageCommitJobStore &store_;
    int claim_size_ = 64;
    int worker_count_ = 1;
    int lease_ms_ = 15000;
    UsageCommitWorkerMetrics metrics_;
};

class AsyncStreamUsageSink {
public:
    AsyncStreamUsageSink(UsageCommitJobStore &store, const Config &config);

    bool enqueue_or_commit_direct(const UsageCommitJobInput &input);
    std::vector<long long> flush();
    unsigned long long fallback_sync_total() const;
    size_t queue_depth() const;

private:
    UsageCommitJobStore &store_;
    size_t batch_size_ = 256;
    size_t queue_size_ = 4096;
    std::vector<UsageCommitJobInput> queued_;
    unsigned long long fallback_sync_total_ = 0;
};

std::string usage_commit_timestamp_now();
void run_usage_commit_runtime_tick(UsageCommitJobStore &store, AsyncStreamUsageSink &sink, UsageCommitWorker &worker,
                                   std::string_view stale_before_time);

} // namespace revlm
