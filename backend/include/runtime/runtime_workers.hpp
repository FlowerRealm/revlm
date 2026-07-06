#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "runtime/concurrency.hpp"
#include "server/tokens.hpp"
#include "usage/usage_commit_jobs.hpp"

namespace revlm
{

class AuthResolver {
public:
    explicit AuthResolver(Config config);

    std::optional<TokenAuth> resolve_token_auth(std::string_view raw_token);

private:
    struct State;
    std::shared_ptr<State> state_;
};

struct UsageFinalizeSinkMetrics {
    unsigned long long queue_depth = 0;
    unsigned long long flushes = 0;
    unsigned long long persisted_jobs = 0;
    unsigned long long fallback_sync_total = 0;
};

class UsageFinalizeSink {
public:
    UsageFinalizeSink(UsageCommitJobStore &store, const Config &config);

    bool enqueue_or_commit_direct(const UsageCommitJobInput &input);
    std::vector<long long> flush();
    UsageFinalizeSinkMetrics metrics() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

struct UsageCommitRuntimeMetrics {
    unsigned long long ticks = 0;
    unsigned long long stale_aborted_total = 0;
    unsigned long long claim_loops = 0;
    unsigned long long last_claimed = 0;
    unsigned long long last_completed = 0;
    unsigned long long last_dead_lettered = 0;
    unsigned long long last_requeued = 0;
    unsigned long long last_flushed = 0;
    UsageCommitWorkerMetrics worker{};
};

class UsageCommitRuntime {
public:
    UsageCommitRuntime(UsageCommitJobStore &store, UsageFinalizeSink &sink, const Config &config);

    void tick(std::string_view stale_before_time);
    void drain(std::string_view stale_before_time);
    UsageCommitRuntimeMetrics metrics() const;

private:
    UsageCommitJobStore &store_;
    UsageFinalizeSink &sink_;
    UsageCommitWorker worker_;
    UsageCommitRuntimeMetrics metrics_;
};

struct CoordinatorMetrics {
    unsigned long long routing_invalidations = 0;
};

class RuntimeCoordinator {
public:
    RuntimeCoordinator() = default;

    void invalidate_routing();
    CoordinatorMetrics metrics() const;

private:
    std::atomic_ullong routing_invalidations_{ 0 };
};

struct RuntimeWorkerRegistry {
    std::shared_ptr<AuthResolver> auth_resolver;
    std::shared_ptr<RuntimeCoordinator> coordinator;
    std::shared_ptr<CredentialConcurrencyManager> concurrency_manager;
    std::shared_ptr<std::atomic_bool> shutdown_draining;
    std::shared_ptr<std::atomic_ullong> requests_in_flight;
    std::shared_ptr<std::atomic_ullong> usage_finalize_queue_depth;
    std::shared_ptr<std::atomic_ullong> usage_finalize_fallback_sync_total;
    std::shared_ptr<std::atomic_ullong> usage_finalize_flush_total;
    std::shared_ptr<std::atomic_ullong> usage_commit_claimed_total;
    std::shared_ptr<std::atomic_ullong> usage_commit_completed_total;
    std::shared_ptr<std::atomic_ullong> usage_commit_dead_letter_total;
    std::shared_ptr<std::atomic_ullong> usage_commit_requeued_total;
    std::shared_ptr<std::atomic_ullong> usage_commit_stale_aborted_total;
};

void install_runtime_worker_registry(RuntimeWorkerRegistry registry);
void clear_runtime_worker_registry();
RuntimeWorkerRegistry runtime_worker_registry();
CredentialConcurrencyManager *runtime_concurrency_manager();

struct RuntimeMetricsSnapshot {
    unsigned long long requests_in_flight = 0;
    CoordinatorMetrics coordinator{};
    UsageFinalizeSinkMetrics usage_finalize{};
    UsageCommitRuntimeMetrics usage_commit{};
    bool shutdown_draining = false;
};

RuntimeMetricsSnapshot runtime_metrics_snapshot();
std::string runtime_metrics_prometheus_text();
std::string usage_commit_timestamp_at(std::chrono::system_clock::time_point when);

class RequestInFlightGuard {
public:
    RequestInFlightGuard();
    ~RequestInFlightGuard();

private:
    std::shared_ptr<std::atomic_ullong> counter_;
};

std::optional<TokenAuth> resolve_token_auth(const Config &config, std::string_view raw_token);
void notify_runtime_routing_invalidated();

} // namespace revlm
