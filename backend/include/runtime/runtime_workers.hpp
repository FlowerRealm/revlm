#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "config/config.hpp"
#include "server/tokens.hpp"

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

struct RuntimeWorkerRegistry {
    std::shared_ptr<AuthResolver> auth_resolver;
    std::shared_ptr<std::atomic_bool> shutdown_draining;
    std::shared_ptr<std::atomic_ullong> requests_in_flight;
};

void install_runtime_worker_registry(RuntimeWorkerRegistry registry);
void clear_runtime_worker_registry();
RuntimeWorkerRegistry runtime_worker_registry();

struct RuntimeMetricsSnapshot {
    unsigned long long requests_in_flight = 0;
    bool shutdown_draining = false;
};

RuntimeMetricsSnapshot runtime_metrics_snapshot();
std::string runtime_metrics_prometheus_text();

class RequestInFlightGuard {
public:
    RequestInFlightGuard();
    ~RequestInFlightGuard();

private:
    std::shared_ptr<std::atomic_ullong> counter_;
};

std::optional<TokenAuth> resolve_token_auth(const Config &config, std::string_view raw_token);

} // namespace revlm
