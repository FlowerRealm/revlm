#include "channels/channels.hpp"
#include "runtime/runtime_cache.hpp"

#include <iostream>
#include <string>
#include <vector>

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

} // namespace

int main()
{
    revlm::RuntimeCacheCoordinator &coordinator = revlm::runtime_cache_coordinator();
    coordinator.observe(revlm::CacheScope::Routing);

    int calls = 0;
    (void)revlm::runtime_routing_cache().list_channels([&]() {
        calls += 1;
        return std::vector<revlm::Channel>{};
    });
    if (expect(calls == 1, "routing cache should load once") != 0) {
        return 1;
    }
    (void)revlm::runtime_routing_cache().list_channels([&]() {
        calls += 1;
        return std::vector<revlm::Channel>{};
    });
    if (expect(calls == 1, "routing cache should hit on second lookup") != 0) {
        return 1;
    }

    coordinator.mark_dirty(revlm::CacheScope::Routing);
    (void)revlm::runtime_routing_cache().list_channels([&]() {
        calls += 1;
        return std::vector<revlm::Channel>{};
    });
    if (expect(calls == 2, "routing cache should reload after invalidation") != 0) {
        return 1;
    }

    const std::string metadata =
        revlm::runtime_metadata_cache().get_json("probe", []() { return std::string{ "{\"ok\":true}\n" }; });
    if (expect(metadata.find("ok") != std::string::npos, "metadata cache should return payload") != 0) {
        return 1;
    }

    return 0;
}
