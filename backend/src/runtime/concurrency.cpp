#include "runtime/concurrency.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds default_wait_timeout{ std::chrono::seconds(30) };
constexpr std::chrono::milliseconds default_initial_backoff{ 100 };
constexpr std::chrono::milliseconds default_max_backoff{ std::chrono::seconds(2) };
constexpr std::chrono::milliseconds default_slot_ttl{ std::chrono::minutes(30) };
constexpr std::chrono::seconds default_ping_interval{ 10 };

std::chrono::milliseconds derive_wait_ttl(std::chrono::milliseconds wait_timeout, std::chrono::milliseconds max_backoff)
{
    if (wait_timeout <= std::chrono::milliseconds::zero()) {
        wait_timeout = default_wait_timeout;
    }
    if (max_backoff <= std::chrono::milliseconds::zero()) {
        max_backoff = default_max_backoff;
    }
    return wait_timeout + max_backoff + std::chrono::seconds(1);
}

ConcurrencyManagerOptions normalize_options(ConcurrencyManagerOptions options)
{
    options.redis_addr = trim_ascii(options.redis_addr);
    options.redis_password = trim_ascii(options.redis_password);
    options.key_prefix = trim_ascii(options.key_prefix);
    if (options.key_prefix.empty()) {
        options.key_prefix = "revlm";
    }
    if (options.slot_ttl <= std::chrono::milliseconds::zero()) {
        options.slot_ttl = default_slot_ttl;
    }
    if (options.wait_timeout <= std::chrono::milliseconds::zero()) {
        options.wait_timeout = default_wait_timeout;
    }
    if (options.initial_backoff <= std::chrono::milliseconds::zero()) {
        options.initial_backoff = default_initial_backoff;
    }
    if (options.max_backoff <= std::chrono::milliseconds::zero()) {
        options.max_backoff = default_max_backoff;
    }
    if (options.max_backoff < options.initial_backoff) {
        options.max_backoff = options.initial_backoff;
    }
    if (options.wait_ttl <= std::chrono::milliseconds::zero()) {
        options.wait_ttl = derive_wait_ttl(options.wait_timeout, options.max_backoff);
    }
    if (options.wait_queue_extra < 0) {
        options.wait_queue_extra = 0;
    }
    if (options.redis_db < 0) {
        options.redis_db = 0;
    }
    return options;
}

std::string random_hex(size_t bytes)
{
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 255);
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        const int value = dist(rd);
        out.push_back(hex[(value >> 4) & 0x0f]);
        out.push_back(hex[value & 0x0f]);
    }
    return out;
}

std::string random_instance_id()
{
    return random_hex(8);
}

std::chrono::milliseconds next_backoff(std::chrono::milliseconds current, std::chrono::milliseconds initial,
                                       std::chrono::milliseconds max)
{
    if (current <= std::chrono::milliseconds::zero()) {
        current = initial;
    }
    const auto scaled = std::chrono::duration_cast<std::chrono::milliseconds>(current * 3 / 2);
    const auto base = std::min(max, std::max(initial, scaled));
    thread_local std::mt19937 generator{ std::random_device{}() };
    std::uniform_real_distribution<double> jitter(0.8, 1.2);
    const auto jittered = std::chrono::duration_cast<std::chrono::milliseconds>(base * jitter(generator));
    if (jittered < initial) {
        return initial;
    }
    if (jittered > max) {
        return max;
    }
    return jittered;
}

std::string make_scope(std::string_view credential_key)
{
    return "cred:" + trim_ascii(credential_key);
}

struct ParsedRedisAddress {
    std::string host;
    std::string port;
};

ParsedRedisAddress parse_redis_address(std::string_view raw)
{
    std::string value = trim_ascii(raw);
    if (value.empty()) {
        throw std::invalid_argument("redis address must not be empty");
    }
    constexpr std::string_view redis_prefix = "redis://";
    if (value.rfind(redis_prefix.data(), 0) == 0) {
        value.erase(0, redis_prefix.size());
    }
    if (!value.empty() && value.front() == '[') {
        const auto end = value.find(']');
        if (end == std::string::npos || end + 2 > value.size() || value[end + 1] != ':') {
            throw std::invalid_argument("invalid redis address");
        }
        return { value.substr(1, end - 1), value.substr(end + 2) };
    }
    const auto pos = value.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= value.size()) {
        throw std::invalid_argument("redis address must be host:port");
    }
    return { value.substr(0, pos), value.substr(pos + 1) };
}

class RedisValue {
public:
    enum class Type {
        SimpleString,
        BulkString,
        Integer,
        Array,
        Nil,
    };

    Type type = Type::Nil;
    std::string string_value;
    long long integer_value = 0;
    std::vector<RedisValue> array_value;
};

class RedisConnection {
public:
    RedisConnection(std::string addr, std::string password, int db)
        : addr_(std::move(addr))
        , password_(std::move(password))
        , db_(db)
    {
    }

    ~RedisConnection()
    {
        close();
    }

    void connect_and_auth()
    {
        std::lock_guard<std::mutex> lock(mu_);
        ensure_connected_locked();
    }

    RedisValue command(const std::vector<std::string> &parts)
    {
        std::lock_guard<std::mutex> lock(mu_);
        ensure_connected_locked();
        write_command(parts);
        return read_value();
    }

    RedisValue read_reply_only()
    {
        std::lock_guard<std::mutex> lock(mu_);
        ensure_connected_locked();
        return read_value();
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(mu_);
        close_locked();
    }

private:
    void ensure_connected_locked()
    {
        if (fd_ >= 0) {
            return;
        }
        const auto parsed = parse_redis_address(addr_);
        struct addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        struct addrinfo *result = nullptr;
        if (::getaddrinfo(parsed.host.c_str(), parsed.port.c_str(), &hints, &result) != 0) {
            throw std::runtime_error("resolve redis address failed");
        }
        std::unique_ptr<struct addrinfo, decltype(&::freeaddrinfo)> holder(result, ::freeaddrinfo);
        int fd = -1;
        for (struct addrinfo *item = result; item != nullptr; item = item->ai_next) {
            fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
            if (fd < 0) {
                continue;
            }
            if (::connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
                break;
            }
            ::close(fd);
            fd = -1;
        }
        if (fd < 0) {
            throw std::runtime_error("connect redis failed");
        }
        fd_ = fd;

        if (!password_.empty()) {
            write_command({ "AUTH", password_ });
            const auto auth = read_value();
            if (auth.string_value != "OK") {
                throw std::runtime_error("redis auth failed");
            }
        }
        if (db_ > 0) {
            write_command({ "SELECT", std::to_string(db_) });
            const auto select = read_value();
            if (select.string_value != "OK") {
                throw std::runtime_error("redis select failed");
            }
        }
    }

    void close_locked()
    {
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
        buffer_.clear();
    }

    void write_command(const std::vector<std::string> &parts)
    {
        std::string payload = "*" + std::to_string(parts.size()) + "\r\n";
        for (const auto &part : parts) {
            payload += "$" + std::to_string(part.size()) + "\r\n";
            payload += part;
            payload += "\r\n";
        }
        size_t offset = 0;
        while (offset < payload.size()) {
            const ssize_t written = ::send(fd_, payload.data() + offset, payload.size() - offset, MSG_NOSIGNAL);
            if (written <= 0) {
                throw std::runtime_error("redis write failed");
            }
            offset += static_cast<size_t>(written);
        }
    }

    RedisValue read_value()
    {
        const char prefix = read_byte();
        switch (prefix) {
        case '+': {
            RedisValue value;
            value.type = RedisValue::Type::SimpleString;
            value.string_value = read_line();
            return value;
        }
        case '-':
            throw std::runtime_error("redis error: " + read_line());
        case ':': {
            RedisValue value;
            value.type = RedisValue::Type::Integer;
            value.integer_value = std::stoll(read_line());
            return value;
        }
        case '$': {
            const long long len = std::stoll(read_line());
            if (len < 0) {
                return {};
            }
            RedisValue value;
            value.type = RedisValue::Type::BulkString;
            value.string_value = read_exact(static_cast<size_t>(len));
            discard_crlf();
            return value;
        }
        case '*': {
            const long long len = std::stoll(read_line());
            if (len < 0) {
                return {};
            }
            RedisValue value;
            value.type = RedisValue::Type::Array;
            value.array_value.reserve(static_cast<size_t>(len));
            for (long long i = 0; i < len; ++i) {
                value.array_value.push_back(read_value());
            }
            return value;
        }
        default:
            throw std::runtime_error("redis protocol parse failed");
        }
    }

    char read_byte()
    {
        ensure_buffer(1);
        const char ch = buffer_.front();
        buffer_.erase(buffer_.begin());
        return ch;
    }

    std::string read_line()
    {
        while (true) {
            const auto pos = buffer_.find("\r\n");
            if (pos != std::string::npos) {
                std::string line = buffer_.substr(0, pos);
                buffer_.erase(0, pos + 2);
                return line;
            }
            fill_buffer();
        }
    }

    std::string read_exact(size_t len)
    {
        ensure_buffer(len);
        std::string out = buffer_.substr(0, len);
        buffer_.erase(0, len);
        return out;
    }

    void discard_crlf()
    {
        ensure_buffer(2);
        if (buffer_[0] != '\r' || buffer_[1] != '\n') {
            throw std::runtime_error("redis bulk string missing terminator");
        }
        buffer_.erase(0, 2);
    }

    void ensure_buffer(size_t len)
    {
        while (buffer_.size() < len) {
            fill_buffer();
        }
    }

    void fill_buffer()
    {
        char chunk[4096];
        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            throw std::runtime_error("redis read failed");
        }
        buffer_.append(chunk, static_cast<size_t>(n));
    }

    std::string addr_;
    std::string password_;
    int db_ = 0;
    int fd_ = -1;
    std::string buffer_;
    std::mutex mu_;
};

struct ReleaseNotifierWaiter {
    std::condition_variable cv;
    std::string scope;
    bool queued = false;
    bool signaled = false;
    bool stopped = false;
    std::list<std::shared_ptr<ReleaseNotifierWaiter>>::iterator iter;
};

class ReleaseNotifier {
public:
    std::shared_ptr<ReleaseNotifierWaiter> subscribe(const std::string &scope)
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto &ready = waiters_[scope];
        auto waiter = std::make_shared<ReleaseNotifierWaiter>();
        waiter->scope = scope;
        waiter->queued = true;
        ready.push_back(waiter);
        waiter->iter = std::prev(ready.end());
        return waiter;
    }

    void rearm(const std::shared_ptr<ReleaseNotifierWaiter> &waiter)
    {
        if (!waiter) {
            return;
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (waiter->stopped || waiter->queued) {
            return;
        }
        auto &ready = waiters_[waiter->scope];
        ready.push_back(waiter);
        waiter->iter = std::prev(ready.end());
        waiter->queued = true;
        waiter->signaled = false;
    }

    void stop(const std::shared_ptr<ReleaseNotifierWaiter> &waiter)
    {
        if (!waiter) {
            return;
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (waiter->stopped) {
            return;
        }
        waiter->stopped = true;
        if (waiter->queued) {
            auto found = waiters_.find(waiter->scope);
            if (found != waiters_.end()) {
                found->second.erase(waiter->iter);
                if (found->second.empty()) {
                    waiters_.erase(found);
                }
            }
            waiter->queued = false;
        }
    }

    void signal(const std::string &scope)
    {
        std::shared_ptr<ReleaseNotifierWaiter> target;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto found = waiters_.find(scope);
            if (found == waiters_.end()) {
                return;
            }
            while (!found->second.empty()) {
                auto waiter = found->second.front();
                found->second.pop_front();
                waiter->queued = false;
                if (waiter->stopped) {
                    continue;
                }
                waiter->signaled = true;
                target = std::move(waiter);
                break;
            }
            if (found->second.empty()) {
                waiters_.erase(found);
            }
        }
        if (target) {
            target->cv.notify_one();
        }
    }

    void close()
    {
        std::vector<std::shared_ptr<ReleaseNotifierWaiter>> notify;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto &[scope, ready] : waiters_) {
                (void)scope;
                for (auto &waiter : ready) {
                    waiter->queued = false;
                    waiter->stopped = true;
                    notify.push_back(waiter);
                }
            }
            waiters_.clear();
        }
        for (auto &waiter : notify) {
            waiter->cv.notify_one();
        }
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, std::list<std::shared_ptr<ReleaseNotifierWaiter>>> waiters_;
};

struct LocalWaiter {
    std::condition_variable cv;
    bool notified = false;
};

struct LocalScopeState {
    int active = 0;
    std::deque<std::shared_ptr<LocalWaiter>> waiters;
};

struct RedisOwnedSlot {
    std::string slot_key;
    std::string scope;
    std::string request_id;
};

std::string parse_release_scope(const std::string &instance_id, std::string payload)
{
    payload = trim_ascii(payload);
    if (payload.empty()) {
        return {};
    }
    const auto split = payload.find(' ');
    if (split == std::string::npos) {
        return payload;
    }
    const auto source = payload.substr(0, split);
    const auto scope = trim_ascii(payload.substr(split + 1));
    if (source == instance_id) {
        return {};
    }
    return scope;
}

std::string redis_string(const RedisValue &value)
{
    return value.string_value;
}

long long redis_int(const RedisValue &value)
{
    return value.integer_value;
}

ConcurrencyAcquireResult error_result(ConcurrencyAcquireError error, std::string message)
{
    ConcurrencyAcquireResult result;
    result.error = error;
    result.message = std::move(message);
    return result;
}

} // namespace

struct CredentialConcurrencyLease::State {
    std::function<void()> releaser;
    std::atomic_bool released{ false };
};

CredentialConcurrencyLease::CredentialConcurrencyLease(std::shared_ptr<State> state)
    : state_(std::move(state))
{
}

CredentialConcurrencyLease::CredentialConcurrencyLease(CredentialConcurrencyLease &&other) noexcept
    : state_(std::move(other.state_))
{
}

CredentialConcurrencyLease &CredentialConcurrencyLease::operator=(CredentialConcurrencyLease &&other) noexcept
{
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
    }
    return *this;
}

CredentialConcurrencyLease::~CredentialConcurrencyLease()
{
    release();
}

void CredentialConcurrencyLease::release()
{
    if (!state_) {
        return;
    }
    const auto state = std::move(state_);
    if (state->released.exchange(true)) {
        return;
    }
    state->releaser();
}

CredentialConcurrencyLease::operator bool() const
{
    return state_ != nullptr;
}

struct CredentialConcurrencyManager::ManagerState
    : public std::enable_shared_from_this<CredentialConcurrencyManager::ManagerState> {
    explicit ManagerState(ConcurrencyManagerOptions raw_options)
        : options(normalize_options(std::move(raw_options)))
        , instance_id(random_instance_id())
    {
    }

    ConcurrencyManagerOptions options;
    const std::string instance_id;
    std::atomic_uint64_t sequence{ 0 };
    std::atomic_bool closed{ false };

    std::mutex local_mu;
    std::unordered_map<std::string, LocalScopeState> local_scopes;

    std::mutex owned_mu;
    std::unordered_map<std::string, RedisOwnedSlot> redis_owned_slots;

    ReleaseNotifier notifier;

    std::unique_ptr<RedisConnection> redis;
    std::unique_ptr<RedisConnection> redis_subscriber;
    std::thread subscriber_thread;

    bool uses_redis() const
    {
        return !options.redis_addr.empty();
    }

    std::string key(const std::string &suffix) const
    {
        return options.key_prefix + ":" + suffix;
    }

    std::string release_channel_key() const
    {
        return key("concurrency:release");
    }

    std::string next_request_id()
    {
        return random_hex(8) + "-" + std::to_string(sequence.fetch_add(1) + 1);
    }

    void ping()
    {
        if (!uses_redis() || !redis) {
            return;
        }
        const auto reply = redis->command({ "PING" });
        if (redis_string(reply) != "PONG") {
            throw std::runtime_error("redis ping failed");
        }
    }

    void start_subscriber_loop()
    {
        if (!uses_redis() || !redis_subscriber) {
            return;
        }
        subscriber_thread = std::thread([self = shared_from_this()] {
            while (!self->closed.load()) {
                try {
                    const auto value = self->redis_subscriber->read_reply_only();
                    if (value.type != RedisValue::Type::Array || value.array_value.size() < 3) {
                        continue;
                    }
                    const auto type = redis_string(value.array_value[0]);
                    if (type != "message") {
                        continue;
                    }
                    const auto payload = redis_string(value.array_value[2]);
                    const auto scope = parse_release_scope(self->instance_id, payload);
                    if (!scope.empty()) {
                        self->notifier.signal(scope);
                    }
                } catch (const std::exception &) {
                    return;
                }
            }
        });
    }

    void ensure_redis_started()
    {
        if (!uses_redis()) {
            return;
        }
        if (!redis) {
            redis = std::make_unique<RedisConnection>(options.redis_addr, options.redis_password, options.redis_db);
            redis->connect_and_auth();
        }
        if (!redis_subscriber) {
            redis_subscriber =
                std::make_unique<RedisConnection>(options.redis_addr, options.redis_password, options.redis_db);
            redis_subscriber->connect_and_auth();
            const auto reply = redis_subscriber->command({ "SUBSCRIBE", release_channel_key() });
            (void)reply;
            start_subscriber_loop();
        }
    }

    ConcurrencyAcquireResult acquire_local(const std::string &scope, int max_concurrency,
                                           const std::function<bool()> &is_cancelled,
                                           const std::function<bool()> &on_ping)
    {
        if (closed.load()) {
            return error_result(ConcurrencyAcquireError::Closed, "concurrency manager closed");
        }
        const int max_waiters = max_concurrency + options.wait_queue_extra;
        auto deadline = Clock::now() + options.wait_timeout;
        auto next_ping_at = Clock::now() + default_ping_interval;
        auto backoff = options.initial_backoff;

        std::unique_lock<std::mutex> lock(local_mu);
        auto &state = local_scopes[scope];
        if (state.active < max_concurrency && state.waiters.empty()) {
            ++state.active;
            return make_local_success(scope);
        }
        if (static_cast<int>(state.waiters.size()) >= max_waiters) {
            return error_result(ConcurrencyAcquireError::QueueFull, "concurrency queue full");
        }

        auto waiter = std::make_shared<LocalWaiter>();
        state.waiters.push_back(waiter);
        while (true) {
            if (closed.load()) {
                remove_local_waiter(scope, waiter);
                return error_result(ConcurrencyAcquireError::Closed, "concurrency manager closed");
            }
            if (is_cancelled && is_cancelled()) {
                remove_local_waiter(scope, waiter);
                return error_result(ConcurrencyAcquireError::Cancelled, "concurrency wait cancelled");
            }
            if (!state.waiters.empty() && state.waiters.front() == waiter && state.active < max_concurrency) {
                state.waiters.pop_front();
                ++state.active;
                if (state.waiters.empty() && state.active == 0) {
                    local_scopes.erase(scope);
                }
                return make_local_success(scope);
            }
            const auto now = Clock::now();
            if (now >= deadline) {
                remove_local_waiter(scope, waiter);
                return error_result(ConcurrencyAcquireError::WaitTimeout, "concurrency wait timeout");
            }

            const auto wait_for =
                std::min(backoff, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            waiter->cv.wait_for(lock, wait_for, [&] { return waiter->notified || closed.load(); });
            waiter->notified = false;
            if (on_ping && Clock::now() >= next_ping_at) {
                lock.unlock();
                const bool keep_waiting = on_ping();
                lock.lock();
                next_ping_at = Clock::now() + default_ping_interval;
                if (!keep_waiting) {
                    remove_local_waiter(scope, waiter);
                    return error_result(ConcurrencyAcquireError::Cancelled, "concurrency wait cancelled");
                }
            }
            backoff = next_backoff(backoff, options.initial_backoff, options.max_backoff);
        }
    }

    ConcurrencyAcquireResult acquire_redis(const std::string &scope, int max_concurrency,
                                           const std::function<bool()> &is_cancelled,
                                           const std::function<bool()> &on_ping)
    {
        ensure_redis_started();
        const std::string slot_key = key("slot:" + scope);
        const std::string wait_key = key("wait:" + scope);
        const std::string request_id = next_request_id();
        const int max_waiters = max_concurrency + options.wait_queue_extra;

        if (try_acquire_redis_slot(slot_key, max_concurrency, request_id)) {
            remember_redis_slot(slot_key, scope, request_id);
            return make_redis_success(slot_key, scope, request_id);
        }
        if (!increment_redis_wait_count(wait_key, max_waiters)) {
            return error_result(ConcurrencyAcquireError::QueueFull, "concurrency queue full");
        }

        bool wait_added = true;
        auto waiter = notifier.subscribe(scope);
        auto stop_waiter = [&] {
            notifier.stop(waiter);
            if (wait_added) {
                wait_added = false;
                decrement_redis_wait_count(wait_key);
            }
        };

        if (try_acquire_redis_slot(slot_key, max_concurrency, request_id)) {
            remember_redis_slot(slot_key, scope, request_id);
            stop_waiter();
            return make_redis_success(slot_key, scope, request_id);
        }

        std::mutex wait_mu;
        std::unique_lock<std::mutex> wait_lock(wait_mu);
        auto deadline = Clock::now() + options.wait_timeout;
        auto next_ping_at = Clock::now() + default_ping_interval;
        auto backoff = options.initial_backoff;
        while (true) {
            if (closed.load()) {
                stop_waiter();
                return error_result(ConcurrencyAcquireError::Closed, "concurrency manager closed");
            }
            if (is_cancelled && is_cancelled()) {
                stop_waiter();
                return error_result(ConcurrencyAcquireError::Cancelled, "concurrency wait cancelled");
            }
            const auto now = Clock::now();
            if (now >= deadline) {
                stop_waiter();
                return error_result(ConcurrencyAcquireError::WaitTimeout, "concurrency wait timeout");
            }

            const auto wait_for =
                std::min(backoff, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            waiter->cv.wait_for(wait_lock, wait_for,
                                [&] { return waiter->signaled || waiter->stopped || closed.load(); });
            if (waiter->signaled) {
                waiter->signaled = false;
                notifier.rearm(waiter);
            }
            if (try_acquire_redis_slot(slot_key, max_concurrency, request_id)) {
                remember_redis_slot(slot_key, scope, request_id);
                stop_waiter();
                return make_redis_success(slot_key, scope, request_id);
            }
            if (on_ping && Clock::now() >= next_ping_at) {
                wait_lock.unlock();
                const bool keep_waiting = on_ping();
                wait_lock.lock();
                next_ping_at = Clock::now() + default_ping_interval;
                if (!keep_waiting) {
                    stop_waiter();
                    return error_result(ConcurrencyAcquireError::Cancelled, "concurrency wait cancelled");
                }
            }
            backoff = next_backoff(backoff, options.initial_backoff, options.max_backoff);
        }
    }

    void release_local_slot(const std::string &scope)
    {
        std::shared_ptr<LocalWaiter> notify;
        std::lock_guard<std::mutex> lock(local_mu);
        auto found = local_scopes.find(scope);
        if (found == local_scopes.end()) {
            return;
        }
        if (found->second.active > 0) {
            --found->second.active;
        }
        if (!found->second.waiters.empty()) {
            notify = found->second.waiters.front();
            notify->notified = true;
        }
        if (found->second.active == 0 && found->second.waiters.empty()) {
            local_scopes.erase(found);
        }
        if (notify) {
            notify->cv.notify_one();
        }
    }

    void release_redis_slot(const std::string &slot_key, const std::string &scope, const std::string &request_id)
    {
        bool should_release = false;
        {
            std::lock_guard<std::mutex> lock(owned_mu);
            auto found = redis_owned_slots.find(request_id);
            if (found != redis_owned_slots.end()) {
                redis_owned_slots.erase(found);
                should_release = true;
            }
        }
        if (!should_release || !redis) {
            return;
        }
        try {
            (void)redis->command(
                { "EVAL", "redis.call('ZREM', KEYS[1], ARGV[1]); return 1", "1", slot_key, request_id });
            signal_release(scope);
        } catch (const std::exception &) {
        }
    }

    void close()
    {
        if (closed.exchange(true)) {
            return;
        }
        notifier.close();
        {
            std::vector<std::shared_ptr<LocalWaiter>> notify;
            std::lock_guard<std::mutex> lock(local_mu);
            for (auto &[scope, state] : local_scopes) {
                (void)scope;
                for (auto &waiter : state.waiters) {
                    waiter->notified = true;
                    notify.push_back(waiter);
                }
                state.waiters.clear();
                state.active = 0;
            }
            local_scopes.clear();
            for (auto &waiter : notify) {
                waiter->cv.notify_one();
            }
        }
        if (redis) {
            std::vector<RedisOwnedSlot> owned;
            {
                std::lock_guard<std::mutex> lock(owned_mu);
                for (const auto &[request_id, slot] : redis_owned_slots) {
                    (void)request_id;
                    owned.push_back(slot);
                }
                redis_owned_slots.clear();
            }
            for (const auto &slot : owned) {
                try {
                    (void)redis->command({ "EVAL", "redis.call('ZREM', KEYS[1], ARGV[1]); return 1", "1", slot.slot_key,
                                           slot.request_id });
                    signal_release(slot.scope);
                } catch (const std::exception &) {
                }
            }
        }
        if (redis_subscriber) {
            redis_subscriber->close();
        }
        if (subscriber_thread.joinable()) {
            subscriber_thread.join();
        }
        if (redis) {
            redis->close();
        }
    }

private:
    ConcurrencyAcquireResult make_local_success(const std::string &scope)
    {
        auto lease_state = std::make_shared<CredentialConcurrencyLease::State>();
        auto self = shared_from_this();
        lease_state->releaser = [self, scope] { self->release_local_slot(scope); };
        return { CredentialConcurrencyLease(std::move(lease_state)), ConcurrencyAcquireError::None, {} };
    }

    ConcurrencyAcquireResult make_redis_success(const std::string &slot_key, const std::string &scope,
                                                const std::string &request_id)
    {
        auto lease_state = std::make_shared<CredentialConcurrencyLease::State>();
        auto self = shared_from_this();
        lease_state->releaser = [self, slot_key, scope, request_id] {
            self->release_redis_slot(slot_key, scope, request_id);
        };
        return { CredentialConcurrencyLease(std::move(lease_state)), ConcurrencyAcquireError::None, {} };
    }

    void remove_local_waiter(const std::string &scope, const std::shared_ptr<LocalWaiter> &waiter)
    {
        auto found = local_scopes.find(scope);
        if (found == local_scopes.end()) {
            return;
        }
        auto &waiters = found->second.waiters;
        const auto iter = std::find(waiters.begin(), waiters.end(), waiter);
        if (iter != waiters.end()) {
            waiters.erase(iter);
        }
        if (found->second.active == 0 && found->second.waiters.empty()) {
            local_scopes.erase(found);
        }
    }

    bool try_acquire_redis_slot(const std::string &slot_key, int max_concurrency, const std::string &request_id)
    {
        const auto script = "local key = KEYS[1] "
                            "local now = tonumber(ARGV[1]) "
                            "local ttl = tonumber(ARGV[2]) "
                            "local max = tonumber(ARGV[3]) "
                            "local req = ARGV[4] "
                            "redis.call('ZREMRANGEBYSCORE', key, '-inf', now-ttl) "
                            "local current = redis.call('ZCARD', key) "
                            "if current >= max then return 0 end "
                            "redis.call('ZADD', key, now, req) "
                            "redis.call('PEXPIRE', key, ttl*2) "
                            "return 1";
        const auto reply =
            redis->command({ "EVAL", script, "1", slot_key,
                             std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count()),
                             std::to_string(options.slot_ttl.count()), std::to_string(max_concurrency), request_id });
        return redis_int(reply) == 1;
    }

    bool increment_redis_wait_count(const std::string &wait_key, int max_waiters)
    {
        const auto script = "local key = KEYS[1] "
                            "local max = tonumber(ARGV[1]) "
                            "local ttl = tonumber(ARGV[2]) "
                            "local current = tonumber(redis.call('GET', key) or '0') "
                            "if current >= max then return 0 end "
                            "current = redis.call('INCR', key) "
                            "redis.call('PEXPIRE', key, ttl) "
                            "return 1";
        const auto reply = redis->command(
            { "EVAL", script, "1", wait_key, std::to_string(max_waiters), std::to_string(options.wait_ttl.count()) });
        return redis_int(reply) == 1;
    }

    void decrement_redis_wait_count(const std::string &wait_key)
    {
        if (!redis) {
            return;
        }
        try {
            const auto script = "local key = KEYS[1] "
                                "local current = tonumber(redis.call('GET', key) or '0') "
                                "if current <= 1 then redis.call('DEL', key) return 0 end "
                                "return redis.call('DECR', key)";
            (void)redis->command({ "EVAL", script, "1", wait_key });
        } catch (const std::exception &) {
        }
    }

    void remember_redis_slot(const std::string &slot_key, const std::string &scope, const std::string &request_id)
    {
        std::lock_guard<std::mutex> lock(owned_mu);
        redis_owned_slots.emplace(request_id, RedisOwnedSlot{ slot_key, scope, request_id });
    }

    void signal_release(const std::string &scope)
    {
        notifier.signal(scope);
        if (!redis) {
            return;
        }
        try {
            (void)redis->command({ "PUBLISH", release_channel_key(), instance_id + " " + scope });
        } catch (const std::exception &) {
        }
    }
};

CredentialConcurrencyManager::CredentialConcurrencyManager(ConcurrencyManagerOptions options)
    : state_(std::make_shared<ManagerState>(std::move(options)))
{
    if (state_->uses_redis()) {
        state_->ensure_redis_started();
    }
}

CredentialConcurrencyManager::~CredentialConcurrencyManager()
{
    close();
}

bool CredentialConcurrencyManager::enabled() const
{
    return state_ != nullptr;
}

bool CredentialConcurrencyManager::uses_redis() const
{
    return state_ != nullptr && state_->uses_redis();
}

void CredentialConcurrencyManager::ping()
{
    if (state_) {
        state_->ping();
    }
}

void CredentialConcurrencyManager::close()
{
    if (state_) {
        state_->close();
    }
}

ConcurrencyAcquireResult
CredentialConcurrencyManager::acquire_credential_slot_with_wait(std::string_view credential_key, int max_concurrency,
                                                                const std::function<bool()> &is_cancelled,
                                                                const std::function<bool()> &on_ping)
{
    if (!state_ || max_concurrency <= 0) {
        return {};
    }
    const std::string scope = make_scope(credential_key);
    if (scope == "cred:") {
        return {};
    }
    try {
        if (state_->uses_redis()) {
            return state_->acquire_redis(scope, max_concurrency, is_cancelled, on_ping);
        }
        return state_->acquire_local(scope, max_concurrency, is_cancelled, on_ping);
    } catch (const std::exception &err) {
        return error_result(ConcurrencyAcquireError::BackendError, err.what());
    }
}

std::unique_ptr<CredentialConcurrencyManager> make_credential_concurrency_manager(const Config &config)
{
    if (config.gateway_credential_max_concurrency <= 0) {
        return nullptr;
    }
    ConcurrencyManagerOptions options;
    options.redis_addr = config.redis_addr;
    options.redis_password = config.redis_password;
    options.redis_db = config.redis_db;
    options.key_prefix = config.redis_key_prefix;
    options.wait_timeout = std::chrono::milliseconds(config.gateway_wait_timeout_ms);
    options.wait_ttl =
        std::chrono::milliseconds(config.gateway_wait_timeout_ms + config.gateway_retry_max_delay_ms + 1000);
    options.initial_backoff = std::chrono::milliseconds(config.gateway_retry_base_delay_ms);
    options.max_backoff = std::chrono::milliseconds(config.gateway_retry_max_delay_ms);
    options.wait_queue_extra = config.gateway_wait_queue_extra_slots;
    return std::make_unique<CredentialConcurrencyManager>(std::move(options));
}

} // namespace revlm
