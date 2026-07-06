#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "errors/errors.hpp"
#include "proxy_response/gateway_stream.hpp"
#include "runtime/concurrency.hpp"
#include "scheduler/scheduler.hpp"

namespace revlm
{

struct Config;

struct GatewayRetryPolicy {
    int max_attempts = 1;
    int max_switches = 0;
    int max_elapsed_ms = 0;
};

class GatewayRetryBudget {
public:
    explicit GatewayRetryBudget(GatewayRetryPolicy policy);

    bool can_attempt(bool switching) const;
    void note_attempt(bool switched);

    int attempts() const
    {
        return attempts_;
    }
    int switches() const
    {
        return switches_;
    }
    int elapsed_ms() const;

private:
    GatewayRetryPolicy policy_;
    std::chrono::steady_clock::time_point started_at_;
    int attempts_ = 0;
    int switches_ = 0;
};

struct GatewayFailure {
    bool retriable = false;
    bool preserve_upstream_response = false;
    int status_code = 502;
    std::string error_class;
    std::string error_message;
    SchedulerFailureScope failure_scope = SchedulerFailureScope::credential;
};

struct GatewayAttemptTransportError {
    std::string stage;
    std::string message;
};

GatewayFailure classify_gateway_status_failure(int status_code);
GatewayFailure classify_gateway_transport_failure(std::string_view stage, std::string_view message = {});
GatewayFailure classify_gateway_stream_failure(const GatewayStreamPump &pump, int upstream_status_code);
SchedulerResult gateway_failure_to_scheduler_result(const GatewayFailure &failure);
size_t best_gateway_failure_index(const std::vector<GatewayFailure> &failures);

GatewayFailure classify_gateway_concurrency_failure(ConcurrencyAcquireError error, std::string_view message = {});

class GatewayCredentialSlotGuard {
public:
    GatewayCredentialSlotGuard(const Config &config, const SchedulerSelection &selection);
    bool ok() const
    {
        return !failure_.has_value();
    }
    const GatewayFailure &failure() const
    {
        return failure_.value();
    }

private:
    CredentialConcurrencyLease lease_;
    std::optional<GatewayFailure> failure_;
};

} // namespace revlm
