#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "config/config.hpp"
#include "errors/errors.hpp"

namespace revlm
{

class CredentialConcurrencyLease {
    struct State;

public:
    CredentialConcurrencyLease() = default;
    explicit CredentialConcurrencyLease(std::shared_ptr<State> state);
    CredentialConcurrencyLease(CredentialConcurrencyLease &&other) noexcept;
    CredentialConcurrencyLease &operator=(CredentialConcurrencyLease &&other) noexcept;
    CredentialConcurrencyLease(const CredentialConcurrencyLease &) = delete;
    CredentialConcurrencyLease &operator=(const CredentialConcurrencyLease &) = delete;
    ~CredentialConcurrencyLease();

    void release();
    explicit operator bool() const;

private:
    std::shared_ptr<State> state_;

    friend class CredentialConcurrencyManager;
};

struct ConcurrencyAcquireResult {
    CredentialConcurrencyLease lease;
    ConcurrencyAcquireError error = ConcurrencyAcquireError::None;
    std::string message;

    bool ok() const
    {
        return error == ConcurrencyAcquireError::None;
    }
};

struct ConcurrencyManagerOptions {
    std::string redis_addr;
    std::string redis_password;
    int redis_db = 0;
    std::string key_prefix = "revlm";
    std::chrono::milliseconds slot_ttl{ std::chrono::minutes(30) };
    std::chrono::milliseconds wait_ttl{ 0 };
    std::chrono::milliseconds wait_timeout{ std::chrono::seconds(30) };
    std::chrono::milliseconds initial_backoff{ std::chrono::milliseconds(100) };
    std::chrono::milliseconds max_backoff{ std::chrono::seconds(2) };
    int wait_queue_extra = 20;
};

class CredentialConcurrencyManager {
public:
    explicit CredentialConcurrencyManager(ConcurrencyManagerOptions options);
    ~CredentialConcurrencyManager();

    CredentialConcurrencyManager(const CredentialConcurrencyManager &) = delete;
    CredentialConcurrencyManager &operator=(const CredentialConcurrencyManager &) = delete;

    bool enabled() const;
    bool uses_redis() const;
    void ping();
    void close();

    ConcurrencyAcquireResult acquire_credential_slot_with_wait(std::string_view credential_key, int max_concurrency,
                                                               const std::function<bool()> &is_cancelled = {},
                                                               const std::function<bool()> &on_ping = {});

private:
    struct ManagerState;

    std::shared_ptr<ManagerState> state_;
};

std::unique_ptr<CredentialConcurrencyManager> make_credential_concurrency_manager(const Config &config);

} // namespace revlm
