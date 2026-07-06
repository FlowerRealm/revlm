#include "proxy_request/gateway_resilience.hpp"
#include "config/config.hpp"

#include "runtime/runtime_workers.hpp"

#include <algorithm>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

int failure_rank(const GatewayFailure &failure)
{
    int rank = failure.preserve_upstream_response ? 100 : 0;
    if (!failure.retriable) {
        rank += 1000;
    }
    if (failure.status_code == 429) {
        rank += 200;
    } else if (failure.status_code >= 500) {
        rank += 150;
    } else if (failure.status_code >= 400) {
        rank += 100;
    }
    return rank;
}

} // namespace

GatewayRetryBudget::GatewayRetryBudget(GatewayRetryPolicy policy)
    : policy_(std::move(policy))
    , started_at_(std::chrono::steady_clock::now())
{
    if (policy_.max_attempts <= 0) {
        policy_.max_attempts = 1;
    }
    if (policy_.max_switches < 0) {
        policy_.max_switches = 0;
    }
    if (policy_.max_elapsed_ms < 0) {
        policy_.max_elapsed_ms = 0;
    }
}

bool GatewayRetryBudget::can_attempt(bool switching) const
{
    if (attempts_ >= policy_.max_attempts) {
        return false;
    }
    if (switching && switches_ >= policy_.max_switches) {
        return false;
    }
    if (policy_.max_elapsed_ms > 0 && elapsed_ms() >= policy_.max_elapsed_ms) {
        return false;
    }
    return true;
}

void GatewayRetryBudget::note_attempt(bool switched)
{
    ++attempts_;
    if (switched) {
        ++switches_;
    }
}

int GatewayRetryBudget::elapsed_ms() const
{
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at_).count());
}

GatewayFailure classify_gateway_status_failure(int status_code)
{
    GatewayFailure failure;
    failure.status_code = status_code > 0 ? status_code : 502;
    failure.error_class = "upstream_status";
    failure.error_message = "upstream returned status " + std::to_string(failure.status_code);
    failure.preserve_upstream_response = failure.status_code >= 400;

    if (failure.status_code == 404 || failure.status_code == 405) {
        failure.retriable = true;
        failure.failure_scope = SchedulerFailureScope::channel;
        return failure;
    }
    if (failure.status_code == 408 || failure.status_code == 409 || failure.status_code == 425) {
        failure.retriable = true;
        failure.failure_scope = SchedulerFailureScope::channel;
        return failure;
    }
    if (failure.status_code == 429) {
        failure.retriable = true;
        failure.failure_scope = SchedulerFailureScope::credential;
        return failure;
    }
    if (failure.status_code >= 500) {
        failure.retriable = true;
        failure.failure_scope = SchedulerFailureScope::channel;
        return failure;
    }
    if (failure.status_code == 401 || failure.status_code == 402 || failure.status_code == 403) {
        failure.failure_scope = SchedulerFailureScope::credential;
    } else {
        failure.failure_scope = SchedulerFailureScope::channel;
    }
    return failure;
}

GatewayFailure classify_gateway_transport_failure(std::string_view stage, std::string_view message)
{
    GatewayFailure failure;
    failure.retriable = true;
    failure.status_code = 502;
    failure.failure_scope = SchedulerFailureScope::channel;

    const std::string clean_stage = trim_ascii(stage);
    if (clean_stage == "parse") {
        failure.error_class = "invalid_upstream_url";
        failure.error_message = "upstream URL is invalid";
        failure.failure_scope = SchedulerFailureScope::channel;
    } else if (clean_stage == "connect") {
        failure.error_class = "connect_upstream";
        failure.error_message = "upstream connect failed";
    } else if (clean_stage == "write") {
        failure.error_class = "write_upstream";
        failure.error_message = "upstream write failed";
    } else if (clean_stage == "read") {
        failure.error_class = "read_upstream";
        failure.error_message = "upstream read failed";
    } else {
        failure.error_class = "network";
        failure.error_message = "upstream unavailable";
    }

    const std::string clean_message = trim_ascii(message);
    if (!clean_message.empty()) {
        failure.error_message = clean_message;
    }
    return failure;
}

GatewayFailure classify_gateway_stream_failure(const GatewayStreamPump &pump, int upstream_status_code)
{
    GatewayFailure failure;
    failure.retriable = true;
    failure.status_code = upstream_status_code > 0 ? upstream_status_code : 502;
    failure.failure_scope = SchedulerFailureScope::channel;

    if (pump.idle_timeout) {
        failure.error_class = "stream_idle_timeout";
        failure.error_message = "upstream stream idle timeout";
        return failure;
    }
    if (pump.upstream_error) {
        failure.error_class = "stream_read_error";
        failure.error_message = "upstream stream read failed";
        return failure;
    }
    failure.error_class = "stream_incomplete";
    failure.error_message = "upstream stream ended before completion";
    return failure;
}

SchedulerResult gateway_failure_to_scheduler_result(const GatewayFailure &failure)
{
    SchedulerResult result;
    result.success = false;
    result.retriable = failure.retriable;
    result.status_code = failure.status_code;
    result.error_class = failure.error_class;
    result.failure_scope = failure.failure_scope;
    return result;
}

size_t best_gateway_failure_index(const std::vector<GatewayFailure> &failures)
{
    size_t best = 0;
    int best_rank = -1;
    for (size_t i = 0; i < failures.size(); ++i) {
        const int rank = failure_rank(failures[i]);
        if (rank >= best_rank) {
            best = i;
            best_rank = rank;
        }
    }
    return best;
}

GatewayFailure classify_gateway_concurrency_failure(ConcurrencyAcquireError error, std::string_view message)
{
    GatewayFailure failure;
    failure.retriable = false;
    failure.preserve_upstream_response = false;
    failure.failure_scope = SchedulerFailureScope::credential;

    switch (error) {
    case ConcurrencyAcquireError::QueueFull:
        failure.status_code = 429;
        failure.error_class = "local_throttled";
        failure.error_message = "并发等待队列已满";
        return failure;
    case ConcurrencyAcquireError::WaitTimeout:
        failure.status_code = 429;
        failure.error_class = "local_throttled";
        failure.error_message = "并发等待超时";
        return failure;
    case ConcurrencyAcquireError::Cancelled:
    case ConcurrencyAcquireError::Closed:
        failure.status_code = 503;
        failure.error_class = "local_throttled";
        failure.error_message = "并发等待已取消";
        return failure;
    case ConcurrencyAcquireError::BackendError:
        failure.status_code = 502;
        failure.error_class = "network";
        failure.error_message = trim_ascii(message);
        if (failure.error_message.empty()) {
            failure.error_message = "concurrency backend error";
        }
        return failure;
    case ConcurrencyAcquireError::None:
        break;
    }

    failure.status_code = 502;
    failure.error_class = "network";
    failure.error_message = trim_ascii(message);
    if (failure.error_message.empty()) {
        failure.error_message = "concurrency acquire failed";
    }
    return failure;
}

GatewayCredentialSlotGuard::GatewayCredentialSlotGuard(const Config &config, const SchedulerSelection &selection)
{
    if (config.gateway_credential_max_concurrency <= 0) {
        return;
    }
    const std::string credential_key = selection.credential_key();
    if (credential_key.empty()) {
        return;
    }
    CredentialConcurrencyManager *manager = runtime_concurrency_manager();
    if (manager == nullptr) {
        return;
    }
    auto acquired =
        manager->acquire_credential_slot_with_wait(credential_key, config.gateway_credential_max_concurrency);
    if (!acquired.ok()) {
        failure_ = classify_gateway_concurrency_failure(acquired.error, acquired.message);
        return;
    }
    lease_ = std::move(acquired.lease);
}

} // namespace revlm
