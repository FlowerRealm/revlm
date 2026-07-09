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
    auto shutdown_draining = std::make_shared<std::atomic_bool>(true);

    revlm::RuntimeWorkerRegistry registry;
    registry.coordinator = coordinator;
    registry.shutdown_draining = shutdown_draining;
    registry.requests_in_flight = requests_in_flight;
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
    if (expect(contains(metrics, "revlm_v1_requests_in_flight 0"), "metrics should expose in-flight requests") != 0) {
        revlm::clear_runtime_worker_registry();
        return 1;
    }

    const auto snapshot = revlm::runtime_metrics_snapshot();
    if (expect(snapshot.shutdown_draining, "snapshot should reflect shutdown drain flag") != 0 ||
        expect(snapshot.requests_in_flight == 0, "snapshot should reflect in-flight requests") != 0) {
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
