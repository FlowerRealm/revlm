#include "runtime/concurrency.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace
{

using namespace std::chrono_literals;

int expect(bool ok, const char *message)
{
    if (ok) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

int test_fifo_and_queue_cleanup()
{
    revlm::ConcurrencyManagerOptions options;
    options.wait_timeout = 400ms;
    options.initial_backoff = 5ms;
    options.max_backoff = 5ms;
    options.wait_queue_extra = 1;
    revlm::CredentialConcurrencyManager manager(options);

    auto holder = manager.acquire_credential_slot_with_wait("cred-A", 1);
    if (!holder.ok()) {
        std::cerr << "holder acquire failed: " << holder.message << '\n';
        return 1;
    }

    std::atomic<int> order{ 0 };
    std::atomic<bool> waiter1_started{ false };
    std::atomic<revlm::ConcurrencyAcquireError> waiter1_err{ revlm::ConcurrencyAcquireError::BackendError };
    std::atomic<revlm::ConcurrencyAcquireError> waiter2_err{ revlm::ConcurrencyAcquireError::BackendError };

    std::thread waiter1([&] {
        waiter1_started.store(true);
        auto result = manager.acquire_credential_slot_with_wait("cred-A", 1);
        waiter1_err.store(result.error);
        if (result.ok()) {
            int expected = 0;
            order.compare_exchange_strong(expected, 1);
            std::this_thread::sleep_for(30ms);
            result.lease.release();
        }
    });

    while (!waiter1_started.load()) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(20ms);

    std::thread waiter2([&] {
        auto result = manager.acquire_credential_slot_with_wait("cred-A", 1);
        waiter2_err.store(result.error);
        if (result.ok()) {
            if (order.load() == 1) {
                order.store(2);
            }
            result.lease.release();
        }
    });

    std::this_thread::sleep_for(40ms);
    auto queue_full = manager.acquire_credential_slot_with_wait("cred-A", 1);
    if (expect(queue_full.error == revlm::ConcurrencyAcquireError::QueueFull,
               "third waiter should be rejected when queue extra slot is full") != 0) {
        holder.lease.release();
        waiter1.join();
        waiter2.join();
        return 1;
    }

    holder.lease.release();
    waiter1.join();
    waiter2.join();

    if (expect(waiter1_err.load() == revlm::ConcurrencyAcquireError::None,
               "first waiter should acquire after holder release") != 0 ||
        expect(waiter2_err.load() == revlm::ConcurrencyAcquireError::None, "second waiter should eventually acquire") !=
            0 ||
        expect(order.load() == 2, "waiters should acquire in FIFO order") != 0) {
        return 1;
    }

    auto reacquire = manager.acquire_credential_slot_with_wait("cred-A", 1);
    if (expect(reacquire.ok(), "queue bookkeeping should be released after waiters complete") != 0) {
        return 1;
    }
    reacquire.lease.release();
    return 0;
}

int test_timeout_and_close()
{
    revlm::ConcurrencyManagerOptions options;
    options.wait_timeout = 80ms;
    options.initial_backoff = 5ms;
    options.max_backoff = 5ms;
    options.wait_queue_extra = 1;
    revlm::CredentialConcurrencyManager manager(options);

    auto holder = manager.acquire_credential_slot_with_wait("cred-B", 1);
    if (!holder.ok()) {
        std::cerr << "holder acquire failed: " << holder.message << '\n';
        return 1;
    }

    const auto timed_out = manager.acquire_credential_slot_with_wait("cred-B", 1);
    if (expect(timed_out.error == revlm::ConcurrencyAcquireError::WaitTimeout,
               "waiter should time out when slot stays occupied") != 0) {
        holder.lease.release();
        return 1;
    }

    std::atomic<revlm::ConcurrencyAcquireError> wait_err{ revlm::ConcurrencyAcquireError::BackendError };
    std::thread waiter([&] {
        auto result = manager.acquire_credential_slot_with_wait("cred-B", 1);
        wait_err.store(result.error);
    });
    std::this_thread::sleep_for(20ms);
    manager.close();
    waiter.join();

    if (expect(wait_err.load() == revlm::ConcurrencyAcquireError::Closed,
               "closing manager should wake and fail blocked waiters") != 0) {
        return 1;
    }

    holder.lease.release();
    return 0;
}

int test_optional_redis_smoke()
{
    const char *redis_addr = std::getenv("REVLM_TEST_REDIS_ADDR");
    if (redis_addr == nullptr || std::string(redis_addr).empty()) {
        return 0;
    }

    revlm::ConcurrencyManagerOptions options;
    options.redis_addr = redis_addr;
    options.key_prefix = "tmp-revlm-s006";
    options.wait_timeout = 400ms;
    options.initial_backoff = 10ms;
    options.max_backoff = 20ms;
    options.wait_queue_extra = 1;

    revlm::CredentialConcurrencyManager holder_mgr(options);
    revlm::CredentialConcurrencyManager waiter_mgr(options);
    holder_mgr.ping();
    waiter_mgr.ping();

    auto holder = holder_mgr.acquire_credential_slot_with_wait("cred-redis", 1);
    if (!holder.ok()) {
        std::cerr << "redis holder acquire failed: " << holder.message << '\n';
        return 1;
    }

    std::atomic<revlm::ConcurrencyAcquireError> waiter_err{ revlm::ConcurrencyAcquireError::BackendError };
    std::atomic<bool> waiter_acquired{ false };
    std::thread waiter([&] {
        auto result = waiter_mgr.acquire_credential_slot_with_wait("cred-redis", 1);
        waiter_err.store(result.error);
        if (result.ok()) {
            waiter_acquired.store(true);
            result.lease.release();
        }
    });

    std::this_thread::sleep_for(40ms);
    holder.lease.release();
    waiter.join();
    if (expect(waiter_err.load() == revlm::ConcurrencyAcquireError::None && waiter_acquired.load(),
               "redis waiter should acquire after remote release notification") != 0) {
        return 1;
    }

    auto leak_holder = holder_mgr.acquire_credential_slot_with_wait("cred-redis-close", 1);
    if (!leak_holder.ok()) {
        std::cerr << "redis cleanup holder acquire failed: " << leak_holder.message << '\n';
        return 1;
    }
    holder_mgr.close();
    auto reacquire = waiter_mgr.acquire_credential_slot_with_wait("cred-redis-close", 1);
    if (expect(reacquire.ok(), "manager close should release owned redis slots") != 0) {
        return 1;
    }
    reacquire.lease.release();
    waiter_mgr.close();
    return 0;
}

} // namespace

int main()
{
    if (test_fifo_and_queue_cleanup() != 0) {
        return 1;
    }
    if (test_timeout_and_close() != 0) {
        return 1;
    }
    if (test_optional_redis_smoke() != 0) {
        return 1;
    }
    return 0;
}
