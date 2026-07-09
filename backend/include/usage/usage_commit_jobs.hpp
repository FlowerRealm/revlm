#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "request/request.hpp"
#include "store/mysql.hpp"

namespace revlm
{

constexpr std::string_view usage_commit_job_state_streaming = "streaming";
constexpr std::string_view usage_commit_job_state_ready = "ready";
constexpr std::string_view usage_commit_job_state_processing = "processing";
constexpr std::string_view usage_commit_job_state_done = "done";
constexpr std::string_view usage_commit_job_state_aborted = "aborted";
constexpr std::string_view usage_commit_job_state_dead_letter = "dead_letter";

struct UsageCommitJob {
    long long id = 0;
    long long usage_event_id = 0;
    long long user_id = 0;
    long long token_id = 0;
    std::string state;
    std::optional<std::string> lease_token;
    std::optional<std::string> lease_until;
    int attempts = 0;
    Request request;
    std::string created_at;
    std::string updated_at;
};

struct UsageCommitJobInput {
    long long usage_event_id = 0;
    long long user_id = 0;
    long long token_id = 0;
    Request request;
    bool direct_commit = false;
    bool balance_debited = false;
    bool retryable = false;
};

struct UsageCommitFinalizeInput {
    long long job_id = 0;
    std::string from_state = std::string{ usage_commit_job_state_streaming };
    std::string to_state = std::string{ usage_commit_job_state_ready };
    Request request;
    bool balance_debited = false;
    bool retryable = false;
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
    std::optional<UsageCommitJob> get_usage_commit_job_by_usage_event_id(long long usage_event_id);
    long long count_usage_events_by_id(long long usage_event_id);
    std::optional<std::string> usage_event_status_by_id(long long usage_event_id);

private:
    bool commit_claimed_job(long long job_id, std::string_view lease_token, std::string_view finished_at,
                            UsageCommitCompletionResult *stats);
    bool write_usage_event(const Request &request, bool balance_debited, std::string_view finished_at);
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
