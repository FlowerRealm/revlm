#include "runtime/runtime_workers.hpp"

#include "auth/users.hpp"
#include "runtime/runtime_cache.hpp"
#include "store/mysql.hpp"

#include <mutex>
#include <sstream>
#include <utility>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

struct RegistryState {
    std::mutex mu;
    RuntimeWorkerRegistry registry;
};

RegistryState &global_registry_state()
{
    static RegistryState state;
    return state;
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
    UserStore::instance().reload(conn);
    TokenStore &store = UserStore::instance().tokens();
    auto auth = store.get_token_auth_by_raw_token(token);
    if (auth.has_value()) {
        auth->groups = store.list_effective_token_channel_groups(auth->token_id);
    }
    return auth;
}

} // namespace

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
    snapshot.shutdown_draining = load_flag(registry.shutdown_draining);
    return snapshot;
}

std::string runtime_metrics_prometheus_text()
{
    const auto metrics = runtime_metrics_snapshot();
    std::ostringstream out;
    out << "revlm_v1_requests_in_flight " << metrics.requests_in_flight << '\n';
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
