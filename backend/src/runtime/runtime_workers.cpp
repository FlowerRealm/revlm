#include "runtime/runtime_workers.hpp"

#include "runtime/runtime_cache.hpp"
#include "store/mysql.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

using SteadyClock = std::chrono::steady_clock;

struct RegistryState {
    std::mutex mu;
    RuntimeWorkerRegistry registry;
};

RegistryState &global_registry_state()
{
    static RegistryState state;
    return state;
}

void publish_usage_finalize_metrics(const UsageFinalizeSinkMetrics &metrics)
{
    const RuntimeWorkerRegistry registry = runtime_worker_registry();
    if (registry.usage_finalize_queue_depth) {
        registry.usage_finalize_queue_depth->store(metrics.queue_depth, std::memory_order_relaxed);
    }
    if (registry.usage_finalize_fallback_sync_total) {
        registry.usage_finalize_fallback_sync_total->store(metrics.fallback_sync_total, std::memory_order_relaxed);
    }
    if (registry.usage_finalize_flush_total) {
        registry.usage_finalize_flush_total->store(metrics.flushes, std::memory_order_relaxed);
    }
}

void publish_usage_commit_metrics(const UsageCommitRuntimeMetrics &metrics)
{
    const RuntimeWorkerRegistry registry = runtime_worker_registry();
    if (registry.usage_commit_claimed_total) {
        registry.usage_commit_claimed_total->store(metrics.worker.claimed_total, std::memory_order_relaxed);
    }
    if (registry.usage_commit_completed_total) {
        registry.usage_commit_completed_total->store(metrics.worker.completed_total, std::memory_order_relaxed);
    }
    if (registry.usage_commit_dead_letter_total) {
        registry.usage_commit_dead_letter_total->store(metrics.worker.dead_letter_total, std::memory_order_relaxed);
    }
    if (registry.usage_commit_requeued_total) {
        registry.usage_commit_requeued_total->store(metrics.worker.requeued_total, std::memory_order_relaxed);
    }
    if (registry.usage_commit_stale_aborted_total) {
        registry.usage_commit_stale_aborted_total->store(metrics.stale_aborted_total, std::memory_order_relaxed);
    }
}

unsigned long long load_counter(const std::shared_ptr<std::atomic_ullong> &counter)
{
    return counter ? counter->load(std::memory_order_relaxed) : 0;
}

bool load_flag(const std::shared_ptr<std::atomic_bool> &flag)
{
    return flag ? flag->load(std::memory_order_relaxed) : false;
}

std::optional<TokenAuth> load_token_auth(const Config &config, std::string_view raw_token)
{
    const std::string token = trim_ascii(raw_token);
    if (token.empty()) {
        return std::nullopt;
    }
    MysqlConnection conn(config.db_dsn);
    TokenStore store(conn);
    auto auth = store.get_token_auth_by_raw_token(token);
    if (auth.has_value()) {
        auth->groups = store.list_effective_token_channel_groups(auth->token_id);
    }
    return auth;
}

} // namespace

std::string usage_commit_timestamp_at(std::chrono::system_clock::time_point when)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(when);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string{ buffer };
}

struct AuthResolver::State {
    explicit State(Config cfg)
        : config(std::move(cfg))
    {
    }

    Config config;
    mutable std::mutex mu;
};

AuthResolver::AuthResolver(Config config)
    : state_(std::make_shared<State>(std::move(config)))
{
}

std::optional<TokenAuth> AuthResolver::resolve_token_auth(std::string_view raw_token)
{
    Config config;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        config = state_->config;
    }
    try {
        return load_token_auth(config, raw_token);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

struct UsageFinalizeSink::State {
    explicit State(UsageCommitJobStore &job_store, const Config &config)
        : sink(job_store, config)
    {
    }

    mutable std::mutex mu;
    AsyncStreamUsageSink sink;
    UsageFinalizeSinkMetrics metrics;
};

UsageFinalizeSink::UsageFinalizeSink(UsageCommitJobStore &store, const Config &config)
    : state_(std::make_shared<State>(store, config))
{
}

bool UsageFinalizeSink::enqueue_or_commit_direct(const UsageCommitJobInput &input)
{
    std::lock_guard<std::mutex> lock(state_->mu);
    const bool ok = state_->sink.enqueue_or_commit_direct(input);
    state_->metrics.fallback_sync_total = state_->sink.fallback_sync_total();
    state_->metrics.queue_depth = state_->sink.queue_depth();
    publish_usage_finalize_metrics(state_->metrics);
    return ok;
}

std::vector<long long> UsageFinalizeSink::flush()
{
    std::lock_guard<std::mutex> lock(state_->mu);
    const auto ids = state_->sink.flush();
    state_->metrics.flushes += 1;
    state_->metrics.persisted_jobs += ids.size();
    state_->metrics.fallback_sync_total = state_->sink.fallback_sync_total();
    state_->metrics.queue_depth = state_->sink.queue_depth();
    publish_usage_finalize_metrics(state_->metrics);
    return ids;
}

UsageFinalizeSinkMetrics UsageFinalizeSink::metrics() const
{
    std::lock_guard<std::mutex> lock(state_->mu);
    UsageFinalizeSinkMetrics metrics = state_->metrics;
    metrics.fallback_sync_total = state_->sink.fallback_sync_total();
    metrics.queue_depth = state_->sink.queue_depth();
    return metrics;
}

UsageCommitRuntime::UsageCommitRuntime(UsageCommitJobStore &store, UsageFinalizeSink &sink, const Config &config)
    : store_(store)
    , sink_(sink)
    , worker_(store, config)
{
}

void UsageCommitRuntime::tick(std::string_view stale_before_time)
{
    const auto flushed = sink_.flush();
    const auto stale_aborted = store_.abort_stale_streaming_usage_commit_jobs(stale_before_time);
    const auto worker_metrics = worker_.drain_once();
    metrics_.ticks += 1;
    metrics_.claim_loops += 1;
    metrics_.last_flushed = flushed.size();
    metrics_.stale_aborted_total += stale_aborted;
    metrics_.worker = worker_metrics;
    metrics_.last_claimed = worker_metrics.claimed_total;
    metrics_.last_completed = worker_metrics.completed_total;
    metrics_.last_dead_lettered = worker_metrics.dead_letter_total;
    metrics_.last_requeued = worker_metrics.requeued_total;
    publish_usage_commit_metrics(metrics_);
}

void UsageCommitRuntime::drain(std::string_view stale_before_time)
{
    tick(stale_before_time);
}

UsageCommitRuntimeMetrics UsageCommitRuntime::metrics() const
{
    return metrics_;
}

void RuntimeCoordinator::invalidate_routing()
{
    runtime_routing_cache().invalidate();
    routing_invalidations_.fetch_add(1, std::memory_order_relaxed);
}

CoordinatorMetrics RuntimeCoordinator::metrics() const
{
    CoordinatorMetrics metrics;
    metrics.routing_invalidations = routing_invalidations_.load(std::memory_order_relaxed);
    return metrics;
}

void install_runtime_worker_registry(RuntimeWorkerRegistry registry)
{
    auto &state = global_registry_state();
    std::lock_guard<std::mutex> lock(state.mu);
    state.registry = std::move(registry);
}

void clear_runtime_worker_registry()
{
    auto &state = global_registry_state();
    std::lock_guard<std::mutex> lock(state.mu);
    state.registry = RuntimeWorkerRegistry{};
}

RuntimeWorkerRegistry runtime_worker_registry()
{
    auto &state = global_registry_state();
    std::lock_guard<std::mutex> lock(state.mu);
    return state.registry;
}

CredentialConcurrencyManager *runtime_concurrency_manager()
{
    const RuntimeWorkerRegistry registry = runtime_worker_registry();
    return registry.concurrency_manager.get();
}

RuntimeMetricsSnapshot runtime_metrics_snapshot()
{
    const RuntimeWorkerRegistry registry = runtime_worker_registry();
    RuntimeMetricsSnapshot snapshot;
    snapshot.requests_in_flight = load_counter(registry.requests_in_flight);
    if (registry.coordinator) {
        snapshot.coordinator = registry.coordinator->metrics();
    }
    snapshot.usage_finalize.queue_depth = load_counter(registry.usage_finalize_queue_depth);
    snapshot.usage_finalize.fallback_sync_total = load_counter(registry.usage_finalize_fallback_sync_total);
    snapshot.usage_finalize.flushes = load_counter(registry.usage_finalize_flush_total);
    snapshot.usage_commit.worker.claimed_total = load_counter(registry.usage_commit_claimed_total);
    snapshot.usage_commit.worker.completed_total = load_counter(registry.usage_commit_completed_total);
    snapshot.usage_commit.worker.dead_letter_total = load_counter(registry.usage_commit_dead_letter_total);
    snapshot.usage_commit.worker.requeued_total = load_counter(registry.usage_commit_requeued_total);
    snapshot.usage_commit.stale_aborted_total = load_counter(registry.usage_commit_stale_aborted_total);
    snapshot.shutdown_draining = load_flag(registry.shutdown_draining);
    return snapshot;
}

std::string runtime_metrics_prometheus_text()
{
    const auto metrics = runtime_metrics_snapshot();
    std::ostringstream out;
    out << "revlm_v1_requests_in_flight " << metrics.requests_in_flight << '\n';
    out << "revlm_usage_finalize_queue_depth " << metrics.usage_finalize.queue_depth << '\n';
    out << "revlm_usage_finalize_flush_total " << metrics.usage_finalize.flushes << '\n';
    out << "revlm_usage_finalize_fallback_sync_total " << metrics.usage_finalize.fallback_sync_total << '\n';
    out << "revlm_usage_commit_claimed_total " << metrics.usage_commit.worker.claimed_total << '\n';
    out << "revlm_usage_commit_completed_total " << metrics.usage_commit.worker.completed_total << '\n';
    out << "revlm_usage_commit_dead_letter_total " << metrics.usage_commit.worker.dead_letter_total << '\n';
    out << "revlm_usage_commit_requeued_total " << metrics.usage_commit.worker.requeued_total << '\n';
    out << "revlm_usage_commit_stale_aborted_total " << metrics.usage_commit.stale_aborted_total << '\n';
    return out.str();
}

RequestInFlightGuard::RequestInFlightGuard()
{
    counter_ = runtime_worker_registry().requests_in_flight;
    if (counter_) {
        counter_->fetch_add(1, std::memory_order_relaxed);
    }
}

RequestInFlightGuard::~RequestInFlightGuard()
{
    if (counter_) {
        counter_->fetch_sub(1, std::memory_order_relaxed);
    }
}

std::optional<TokenAuth> resolve_token_auth(const Config &config, std::string_view raw_token)
{
    const RuntimeWorkerRegistry registry = runtime_worker_registry();
    if (registry.auth_resolver) {
        return registry.auth_resolver->resolve_token_auth(raw_token);
    }
    try {
        return load_token_auth(config, raw_token);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

void notify_runtime_routing_invalidated()
{
    const RuntimeWorkerRegistry registry = runtime_worker_registry();
    if (registry.coordinator) {
        registry.coordinator->invalidate_routing();
    }
}

} // namespace revlm
