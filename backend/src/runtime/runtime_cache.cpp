#include "runtime/runtime_cache.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

constexpr int redis_io_timeout_ms = 200;

int clamp_positive(int value, int fallback)
{
    return value > 0 ? value : fallback;
}

size_t scope_index(CacheScope scope)
{
    return static_cast<size_t>(scope);
}

void promote_generation(std::atomic<uint64_t> &generation, uint64_t candidate)
{
    uint64_t observed = generation.load();
    while (candidate > observed && !generation.compare_exchange_weak(observed, candidate)) {
    }
}

struct RedisReply {
    enum class Type {
        Nil,
        SimpleString,
        BulkString,
        Integer,
        Error,
    };

    Type type = Type::Nil;
    std::string text;
    long long integer = 0;
};

class RedisClient {
public:
    explicit RedisClient(const RuntimeCacheCoordinator::RedisSettings &settings)
        : settings_(settings)
    {
    }

    std::optional<std::string> get(std::string_view key) const
    {
        if (!settings_.enabled) {
            return std::nullopt;
        }
        if (const auto reply = command({ "GET", std::string{ key } }); reply.type == RedisReply::Type::BulkString) {
            return reply.text;
        }
        return std::nullopt;
    }

    bool set_px(std::string_view key, std::string_view value, int ttl_ms) const
    {
        if (!settings_.enabled) {
            return false;
        }
        const auto reply = command({ "SET", std::string{ key }, std::string{ value }, "PX", std::to_string(ttl_ms) });
        return reply.type == RedisReply::Type::SimpleString && reply.text == "OK";
    }

    std::optional<long long> incr(std::string_view key) const
    {
        if (!settings_.enabled) {
            return std::nullopt;
        }
        const auto reply = command({ "INCR", std::string{ key } });
        if (reply.type == RedisReply::Type::Integer) {
            return reply.integer;
        }
        return std::nullopt;
    }

private:
    int connect_socket() const
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *result = nullptr;
        const int rc = ::getaddrinfo(settings_.host.c_str(), std::to_string(settings_.port).c_str(), &hints, &result);
        if (rc != 0) {
            throw std::runtime_error("resolve redis: " + std::string{ gai_strerror(rc) });
        }

        int fd = -1;
        for (addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) {
                continue;
            }
            timeval tv{};
            tv.tv_sec = redis_io_timeout_ms / 1000;
            tv.tv_usec = (redis_io_timeout_ms % 1000) * 1000;
            (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
                break;
            }
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(result);
        if (fd < 0) {
            throw std::runtime_error("connect redis: " + std::string{ std::strerror(errno) });
        }
        return fd;
    }

    static void write_all(int fd, std::string_view data)
    {
        size_t offset = 0;
        while (offset < data.size()) {
            const ssize_t sent = ::send(fd, data.data() + offset, data.size() - offset, 0);
            if (sent <= 0) {
                throw std::runtime_error("write redis");
            }
            offset += static_cast<size_t>(sent);
        }
    }

    static std::string read_line(int fd)
    {
        std::string line;
        char ch = '\0';
        while (true) {
            const ssize_t n = ::recv(fd, &ch, 1, 0);
            if (n <= 0) {
                throw std::runtime_error("read redis line");
            }
            if (ch == '\r') {
                const ssize_t m = ::recv(fd, &ch, 1, 0);
                if (m <= 0 || ch != '\n') {
                    throw std::runtime_error("read redis line ending");
                }
                return line;
            }
            line.push_back(ch);
        }
    }

    static std::string read_exact(int fd, size_t size)
    {
        std::string out(size, '\0');
        size_t offset = 0;
        while (offset < size) {
            const ssize_t n = ::recv(fd, out.data() + offset, size - offset, 0);
            if (n <= 0) {
                throw std::runtime_error("read redis bulk");
            }
            offset += static_cast<size_t>(n);
        }
        return out;
    }

    static RedisReply parse_reply(int fd)
    {
        char prefix = '\0';
        if (::recv(fd, &prefix, 1, 0) <= 0) {
            throw std::runtime_error("read redis prefix");
        }
        if (prefix == '+') {
            return RedisReply{ RedisReply::Type::SimpleString, read_line(fd), 0 };
        }
        if (prefix == '-') {
            return RedisReply{ RedisReply::Type::Error, read_line(fd), 0 };
        }
        if (prefix == ':') {
            return RedisReply{ RedisReply::Type::Integer, "", std::stoll(read_line(fd)) };
        }
        if (prefix == '$') {
            const long long size = std::stoll(read_line(fd));
            if (size < 0) {
                return {};
            }
            std::string payload = read_exact(fd, static_cast<size_t>(size));
            const std::string crlf = read_exact(fd, 2);
            if (crlf != "\r\n") {
                throw std::runtime_error("redis bulk terminator");
            }
            return RedisReply{ RedisReply::Type::BulkString, std::move(payload), 0 };
        }
        throw std::runtime_error("unsupported redis reply");
    }

    static std::string resp_command(const std::vector<std::string> &parts)
    {
        std::ostringstream out;
        out << "*" << parts.size() << "\r\n";
        for (const std::string &part : parts) {
            out << "$" << part.size() << "\r\n" << part << "\r\n";
        }
        return out.str();
    }

    RedisReply command(const std::vector<std::string> &parts) const
    {
        const int fd = connect_socket();
        try {
            if (!settings_.password.empty()) {
                write_all(fd, resp_command({ "AUTH", settings_.password }));
                const RedisReply auth = parse_reply(fd);
                if (auth.type == RedisReply::Type::Error) {
                    throw std::runtime_error("redis auth: " + auth.text);
                }
            }
            if (settings_.db > 0) {
                write_all(fd, resp_command({ "SELECT", std::to_string(settings_.db) }));
                const RedisReply select = parse_reply(fd);
                if (select.type == RedisReply::Type::Error) {
                    throw std::runtime_error("redis select: " + select.text);
                }
            }
            write_all(fd, resp_command(parts));
            RedisReply reply = parse_reply(fd);
            ::close(fd);
            return reply;
        } catch (...) {
            ::close(fd);
            throw;
        }
    }

    RuntimeCacheCoordinator::RedisSettings settings_;
};

} // namespace

RuntimeCacheCoordinator::RuntimeCacheCoordinator()
{
    for (std::atomic<uint64_t> &generation : generations_) {
        generation.store(1);
    }
    for (std::atomic<long long> &synced_at : last_remote_sync_ms_) {
        synced_at.store(0);
    }
}

void RuntimeCacheCoordinator::configure(const Config &config)
{
    Snapshot next;
    next.routing_ttl_ms = clamp_positive(config.routing_refresh_ms, 30000);
    next.usage_ttl_ms = clamp_positive(config.routing_refresh_ms, 30000);

    std::string redis_addr = trim_ascii(config.redis_addr);
    if (!redis_addr.empty()) {
        next.redis.enabled = true;
        next.redis.password = config.redis_password;
        next.redis.key_prefix = trim_ascii(config.redis_key_prefix);
        if (next.redis.key_prefix.empty()) {
            next.redis.key_prefix = "revlm";
        }
        next.redis.db = config.redis_db >= 0 ? config.redis_db : 0;
        const size_t colon = redis_addr.rfind(':');
        if (colon == std::string::npos) {
            next.redis.host = redis_addr;
            next.redis.port = 6379;
        } else {
            next.redis.host = redis_addr.substr(0, colon);
            next.redis.port = std::stoi(redis_addr.substr(colon + 1));
        }
    }

    std::lock_guard<std::mutex> lock(config_mutex_);
    snapshot_ = std::move(next);
}

RuntimeCacheCoordinator::Snapshot RuntimeCacheCoordinator::snapshot() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    return snapshot_;
}

long long RuntimeCacheCoordinator::scope_sync_interval_ms(CacheScope scope, const Snapshot &snapshot) const
{
    switch (scope) {
    case CacheScope::Metadata:
        return 5000;
    case CacheScope::Routing:
        return std::max(100, snapshot.routing_ttl_ms);
    case CacheScope::Usage:
        return std::max(100, snapshot.usage_ttl_ms);
    }
    return 1000;
}

std::string RuntimeCacheCoordinator::scope_name(CacheScope scope) const
{
    switch (scope) {
    case CacheScope::Metadata:
        return "metadata";
    case CacheScope::Routing:
        return "routing";
    case CacheScope::Usage:
        return "usage";
    }
    return "unknown";
}

std::string RuntimeCacheCoordinator::generation_key(CacheScope scope, const Snapshot &snapshot) const
{
    return snapshot.redis.key_prefix + ":generation:" + scope_name(scope);
}

std::string RuntimeCacheCoordinator::shared_key(CacheScope scope, uint64_t generation, std::string_view key,
                                                const Snapshot &snapshot) const
{
    return snapshot.redis.key_prefix + ":cache:" + scope_name(scope) + ":" + std::to_string(generation) + ":" +
           std::string{ key };
}

void RuntimeCacheCoordinator::refresh_remote_generation(CacheScope scope, const Snapshot &snapshot)
{
    if (!snapshot.redis.enabled) {
        return;
    }
    const size_t index = scope_index(scope);
    const long long now = unix_time_ms_now();
    const long long last_sync = last_remote_sync_ms_[index].load();
    if (now - last_sync < scope_sync_interval_ms(scope, snapshot)) {
        return;
    }
    last_remote_sync_ms_[index].store(now);
    try {
        RedisClient redis(snapshot.redis);
        const std::optional<std::string> raw = redis.get(generation_key(scope, snapshot));
        if (!raw.has_value()) {
            return;
        }
        const uint64_t remote = static_cast<uint64_t>(std::stoull(*raw));
        uint64_t current = generations_[index].load();
        while (remote > current && !generations_[index].compare_exchange_weak(current, remote)) {
        }
    } catch (const std::exception &) {
    }
}

uint64_t RuntimeCacheCoordinator::observe(CacheScope scope)
{
    const Snapshot current = snapshot();
    refresh_remote_generation(scope, current);
    return generations_[scope_index(scope)].load();
}

uint64_t RuntimeCacheCoordinator::mark_dirty(CacheScope scope, CacheInvalidationMode mode)
{
    const Snapshot current = snapshot();
    const size_t index = scope_index(scope);
    if (current.redis.enabled) {
        try {
            RedisClient redis(current.redis);
            if (const std::optional<long long> remote = redis.incr(generation_key(scope, current));
                remote.has_value() && *remote > 0) {
                uint64_t next = generations_[index].fetch_add(1) + 1;
                const uint64_t remote_value = static_cast<uint64_t>(*remote);
                promote_generation(generations_[index], remote_value);
                last_remote_sync_ms_[index].store(unix_time_ms_now());
                return std::max(next, generations_[index].load());
            }
            if (mode == CacheInvalidationMode::RequireRemote) {
                throw std::runtime_error("redis generation increment returned no value");
            }
        } catch (const std::exception &) {
            if (mode == CacheInvalidationMode::RequireRemote) {
                throw;
            }
        }
    }
    const uint64_t next = generations_[index].fetch_add(1) + 1;
    last_remote_sync_ms_[index].store(unix_time_ms_now());
    return next;
}

bool RuntimeCacheCoordinator::redis_enabled() const
{
    return snapshot().redis.enabled;
}

std::optional<std::string> RuntimeCacheCoordinator::shared_get(CacheScope scope, uint64_t generation,
                                                               std::string_view key) const
{
    const Snapshot current = snapshot();
    if (!current.redis.enabled) {
        return std::nullopt;
    }
    try {
        RedisClient redis(current.redis);
        return redis.get(shared_key(scope, generation, key, current));
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

bool RuntimeCacheCoordinator::shared_set(CacheScope scope, uint64_t generation, std::string_view key,
                                         std::string_view value, int ttl_ms) const
{
    const Snapshot current = snapshot();
    if (!current.redis.enabled) {
        return false;
    }
    try {
        RedisClient redis(current.redis);
        return redis.set_px(shared_key(scope, generation, key, current), value, ttl_ms);
    } catch (const std::exception &) {
        return false;
    }
}

int RuntimeCacheCoordinator::routing_ttl_ms() const
{
    return snapshot().routing_ttl_ms;
}

int RuntimeCacheCoordinator::usage_ttl_ms() const
{
    return snapshot().usage_ttl_ms;
}

std::string MetadataCache::get_json(std::string_view key, const std::function<std::string()> &loader)
{
    RuntimeCacheCoordinator &coordinator = runtime_cache_coordinator();
    const uint64_t generation = coordinator.observe(CacheScope::Metadata);
    return cache_
        .get_or_load(std::string{ key }, generation, 24 * 60 * 60 * 1000,
                     [&]() { return std::optional<std::string>{ loader() }; })
        .value_or("");
}

void MetadataCache::invalidate()
{
    cache_.clear();
    (void)runtime_cache_coordinator().mark_dirty(CacheScope::Metadata);
}

std::vector<Channel> RoutingCache::list_channels(const std::function<std::vector<Channel>()> &loader)
{
    RuntimeCacheCoordinator &coordinator = runtime_cache_coordinator();
    const uint64_t generation = coordinator.observe(CacheScope::Routing);
    return channels_
        .get_or_load("channels", generation, coordinator.routing_ttl_ms(),
                     [&]() { return std::optional<std::vector<Channel>>{ loader() }; })
        .value_or(std::vector<Channel>{});
}

void RoutingCache::invalidate()
{
    channels_.clear();
    (void)runtime_cache_coordinator().mark_dirty(CacheScope::Routing);
}

std::string UsageCache::get_query(std::string_view key, const std::function<std::string()> &loader)
{
    RuntimeCacheCoordinator &coordinator = runtime_cache_coordinator();
    const uint64_t generation = coordinator.observe(CacheScope::Usage);
    return query_cache_
        .get_or_load("query:" + std::string{ key }, generation, coordinator.usage_ttl_ms(),
                     [&]() { return std::optional<std::string>{ loader() }; })
        .value_or("");
}

std::string UsageCache::get_coverage(std::string_view key, const std::function<std::string()> &loader)
{
    RuntimeCacheCoordinator &coordinator = runtime_cache_coordinator();
    const uint64_t generation = coordinator.observe(CacheScope::Usage);
    return coverage_cache_
        .get_or_load("coverage:" + std::string{ key }, generation, coordinator.usage_ttl_ms(),
                     [&]() { return std::optional<std::string>{ loader() }; })
        .value_or("");
}

void UsageCache::invalidate()
{
    query_cache_.clear();
    coverage_cache_.clear();
    (void)runtime_cache_coordinator().mark_dirty(CacheScope::Usage);
}

RuntimeCacheCoordinator &runtime_cache_coordinator()
{
    static RuntimeCacheCoordinator coordinator;
    return coordinator;
}

MetadataCache &runtime_metadata_cache()
{
    static MetadataCache cache;
    return cache;
}

RoutingCache &runtime_routing_cache()
{
    static RoutingCache cache;
    return cache;
}

UsageCache &runtime_usage_cache()
{
    static UsageCache cache;
    return cache;
}

} // namespace revlm
