#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "channels/channels.hpp"
#include "config/config.hpp"

namespace revlm
{

inline long long unix_time_ms_now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

template <typename T> class SingleFlightGroup {
public:
    template <typename Loader> T do_call(std::string key, Loader &&loader)
    {
        std::shared_ptr<Call> call;
        bool leader = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = calls_.find(key);
            if (it != calls_.end()) {
                call = it->second;
            } else {
                call = std::make_shared<Call>();
                calls_.emplace(std::move(key), call);
                leader = true;
            }
        }

        if (!leader) {
            return call->future.get();
        }

        try {
            T value = loader();
            call->promise.set_value(value);
        } catch (...) {
            call->promise.set_exception(std::current_exception());
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = calls_.begin(); it != calls_.end(); ++it) {
                if (it->second == call) {
                    calls_.erase(it);
                    break;
                }
            }
        }
        return call->future.get();
    }

private:
    struct Call {
        Call()
            : future(promise.get_future().share())
        {
        }

        std::promise<T> promise;
        std::shared_future<T> future;
    };

    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Call>> calls_;
};

template <typename T> class VersionedCache {
public:
    struct LookupResult {
        bool hit = false;
        std::optional<T> value;
    };

    explicit VersionedCache(size_t max_entries = 1024)
        : max_entries_(max_entries)
    {
    }

    LookupResult lookup(std::string_view key, uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const long long now = unix_time_ms_now();
        if (hot_.has_value() && hot_->key == key && hot_->entry.generation == generation &&
            hot_->entry.expires_at_ms > now) {
            return LookupResult{ true, hot_->entry.value };
        }
        auto it = entries_.find(std::string{ key });
        if (it == entries_.end()) {
            return {};
        }
        if (it->second.generation != generation || it->second.expires_at_ms <= now) {
            entries_.erase(it);
            if (hot_.has_value() && hot_->key == key) {
                hot_.reset();
            }
            return {};
        }
        hot_ = HotEntry{ std::string{ key }, it->second };
        return LookupResult{ true, it->second.value };
    }

    void put(std::string key, uint64_t generation, std::optional<T> value, int ttl_ms)
    {
        const long long expires_at = unix_time_ms_now() + ttl_ms;
        std::lock_guard<std::mutex> lock(mutex_);
        Entry entry{ generation, expires_at, std::move(value) };
        hot_ = HotEntry{ key, entry };
        entries_[key] = std::move(entry);
        trim_locked(unix_time_ms_now());
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hot_.reset();
        entries_.clear();
    }

    template <typename Loader>
    std::optional<T> get_or_load(std::string key, uint64_t generation, int ttl_ms, Loader &&loader)
    {
        if (LookupResult cached = lookup(key, generation); cached.hit) {
            return cached.value;
        }
        const std::optional<T> loaded =
            singleflight_.do_call(key + "#" + std::to_string(generation), [&]() { return loader(); });
        put(std::move(key), generation, loaded, ttl_ms);
        return loaded;
    }

private:
    struct Entry {
        uint64_t generation = 0;
        long long expires_at_ms = 0;
        std::optional<T> value;
    };

    struct HotEntry {
        std::string key;
        Entry entry;
    };

    void trim_locked(long long now)
    {
        if (entries_.size() <= max_entries_) {
            return;
        }
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.expires_at_ms <= now) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
            if (entries_.size() <= max_entries_) {
                return;
            }
        }
        while (entries_.size() > max_entries_) {
            entries_.erase(entries_.begin());
        }
    }

    const size_t max_entries_;
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::optional<HotEntry> hot_;
    SingleFlightGroup<std::optional<T>> singleflight_;
};

enum class CacheScope {
    Metadata = 0,
    Routing = 1,
    Usage = 2,
};

enum class CacheInvalidationMode {
    BestEffort = 0,
    RequireRemote = 1,
};

class RuntimeCacheCoordinator {
public:
    struct RedisSettings {
        std::string host;
        int port = 6379;
        std::string password;
        std::string key_prefix = "revlm";
        int db = 0;
        bool enabled = false;
    };

    RuntimeCacheCoordinator();

    void configure(const Config &config);
    uint64_t observe(CacheScope scope);
    uint64_t mark_dirty(CacheScope scope, CacheInvalidationMode mode = CacheInvalidationMode::BestEffort);

    bool redis_enabled() const;
    std::optional<std::string> shared_get(CacheScope scope, uint64_t generation, std::string_view key) const;
    bool shared_set(CacheScope scope, uint64_t generation, std::string_view key, std::string_view value,
                    int ttl_ms) const;

    int routing_ttl_ms() const;
    int usage_ttl_ms() const;

private:
    struct Snapshot {
        int routing_ttl_ms = 30000;
        int usage_ttl_ms = 30000;
        RedisSettings redis;
    };

    Snapshot snapshot() const;
    long long scope_sync_interval_ms(CacheScope scope, const Snapshot &snapshot) const;
    std::string scope_name(CacheScope scope) const;
    std::string generation_key(CacheScope scope, const Snapshot &snapshot) const;
    std::string shared_key(CacheScope scope, uint64_t generation, std::string_view key, const Snapshot &snapshot) const;
    void refresh_remote_generation(CacheScope scope, const Snapshot &snapshot);

    std::array<std::atomic<uint64_t>, 3> generations_;
    std::array<std::atomic<long long>, 3> last_remote_sync_ms_;
    mutable std::mutex config_mutex_;
    Snapshot snapshot_;
};

class MetadataCache {
public:
    std::string get_json(std::string_view key, const std::function<std::string()> &loader);
    void invalidate();

private:
    VersionedCache<std::string> cache_{ 32 };
};

class RoutingCache {
public:
    std::vector<Channel> list_channels(const std::function<std::vector<Channel>()> &loader);
    void invalidate();

private:
    VersionedCache<std::vector<Channel>> channels_{ 16 };
};

RuntimeCacheCoordinator &runtime_cache_coordinator();
MetadataCache &runtime_metadata_cache();
RoutingCache &runtime_routing_cache();

} // namespace revlm
