#pragma once

#include <stdexcept>

namespace revlm
{

struct DomainError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct QuotaInsufficientBalanceError : DomainError {
    QuotaInsufficientBalanceError()
        : DomainError("insufficient balance")
    {
    }
};

enum class ConcurrencyAcquireError {
    None,
    QueueFull,
    WaitTimeout,
    Cancelled,
    Closed,
    BackendError,
};

} // namespace revlm
