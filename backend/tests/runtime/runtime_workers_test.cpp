#include "runtime/runtime_workers.hpp"

#include <atomic>
#include <iostream>
#include <memory>
#include <string>

namespace
{

int expect(bool ok, const char *message)
{
    if (ok) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

bool contains(const std::string &haystack, const char *needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    auto coordinator = std::make_shared<revlm::RuntimeCoordinator>();
    auto requests_in_flight = std::make_shared<std::atomic_ullong>(0);
    auto usage_finalize_queue_depth = std::make_shared<std::atomic_ullong>(7);
    auto usage_finalize_fallback_sync_total = std::make_shared<std::atomic_ullong>(3);
    auto usage_finalize_flush_total = std::make_shared<std::atomic_ullong>(5);
    auto usage_commit_claimed_total = std::make_shared<std::atomic_ullong>(11);
    auto usage_commit_completed_total = std::make_shared<std::atomic_ullong>(9);
    auto usage_commit_dead_letter_total = std::make_shared<std::atomic_ullong>(1);
    auto usage_commit_requeued_total = std::make_shared<std::atomic_ullong>(2);
    auto usage_commit_stale_aborted_total = std::make_shared<std::atomic_ullong>(4);
    auto shutdown_draining = std::make_shared<std::atomic_bool>(true);

    revlm::RuntimeWorkerRegistry registry;
    registry.coordinator = coordinator;
    registry.shutdown_draining = shutdown_draining;
    registry.requests_in_flight = requests_in_flight;
    registry.usage_finalize_queue_depth = usage_finalize_queue_depth;
    registry.usage_finalize_fallback_sync_total = usage_finalize_fallback_sync_total;
    registry.usage_finalize_flush_total = usage_finalize_flush_total;
    registry.usage_commit_claimed_total = usage_commit_claimed_total;
    registry.usage_commit_completed_total = usage_commit_completed_total;
    registry.usage_commit_dead_letter_total = usage_commit_dead_letter_total;
    registry.usage_commit_requeued_total = usage_commit_requeued_total;
    registry.usage_commit_stale_aborted_total = usage_commit_stale_aborted_total;
    revlm::install_runtime_worker_registry(std::move(registry));

    {
        revlm::RequestInFlightGuard guard;
        if (expect(requests_in_flight->load() == 1, "request guard should increment in-flight") != 0) {
            revlm::clear_runtime_worker_registry();
            return 1;
        }
    }
    if (expect(requests_in_flight->load() == 0, "request guard should decrement in-flight") != 0) {
        revlm::clear_runtime_worker_registry();
        return 1;
    }

    const std::string metrics = revlm::runtime_metrics_prometheus_text();
    if (expect(contains(metrics, "revlm_usage_finalize_queue_depth 7"), "metrics should expose finalize queue depth") !=
            0 ||
        expect(contains(metrics, "revlm_usage_commit_claimed_total 11"),
               "metrics should expose commit claimed total") != 0 ||
        expect(contains(metrics, "revlm_usage_commit_stale_aborted_total 4"),
               "metrics should expose stale aborted total") != 0) {
        revlm::clear_runtime_worker_registry();
        return 1;
    }

    const auto snapshot = revlm::runtime_metrics_snapshot();
    if (expect(snapshot.shutdown_draining, "snapshot should reflect shutdown drain flag") != 0 ||
        expect(snapshot.usage_finalize.queue_depth == 7, "snapshot should reflect finalize queue depth") != 0 ||
        expect(snapshot.usage_commit.worker.completed_total == 9, "snapshot should reflect completed total") != 0) {
        revlm::clear_runtime_worker_registry();
        return 1;
    }

    revlm::notify_runtime_routing_invalidated();
    if (expect(coordinator->metrics().routing_invalidations == 1,
               "routing invalidation should flow through coordinator") != 0) {
        revlm::clear_runtime_worker_registry();
        return 1;
    }

    revlm::clear_runtime_worker_registry();
    return 0;
}
