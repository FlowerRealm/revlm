#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "channels/channels.hpp"

namespace revlm
{

enum class SchedulerApi {
    openai,
    anthropic,
};

enum class SchedulerFailureScope {
    credential,
    channel,
    model,
};

struct SchedulerSelection {
    long long channel_id = 0;
    std::string channel_type;
    double route_group_multiplier = 1.0;
    std::optional<std::string> openai_organization;

    std::string base_url;
    std::string api_key;

    std::string model_public_id;
    long long model_binding_id = 0;

    std::string credential_key() const;
};

struct SchedulerResult {
    bool success = false;
    bool retriable = false;
    int status_code = 0;
    std::string error_class;
    SchedulerFailureScope failure_scope = SchedulerFailureScope::credential;
    std::optional<std::chrono::system_clock::time_point> cooldown_until;
};

struct SchedulerConstraints {
    std::optional<SchedulerApi> required_api;
    std::optional<std::string> required_channel_type;
    long long required_channel_id = 0;
    std::unordered_set<long long> allowed_channel_ids;
    std::unordered_set<long long> excluded_channel_ids;
    std::unordered_set<long long> soft_excluded_channel_ids;
    std::optional<std::string> requested_model;
    bool allow_banned_required_channel = false;
};

struct SchedulerBanEntry {
    std::chrono::system_clock::time_point until;
    int streak = 0;
};

struct SchedulerRoutingSnapshot {
    std::uint64_t generation = 0;
    std::chrono::system_clock::time_point loaded_at;
    std::vector<Channel> channels;

    bool channel_supports_model(long long channel_id, std::string_view public_id) const;
};

class SchedulerRoutingDataSource {
public:
    virtual ~SchedulerRoutingDataSource() = default;

    virtual std::vector<Channel> list_channels() = 0;
};

class SchedulerRedisStateStore {
public:
    virtual ~SchedulerRedisStateStore() = default;

    virtual std::vector<std::string> scan_prefix(std::string_view prefix) = 0;
    virtual std::optional<std::string> get(std::string_view key) = 0;
    virtual void set(std::string_view key, std::string_view value) = 0;
    virtual void del(std::string_view key) = 0;
};

class SchedulerState {
public:
    SchedulerState();
    ~SchedulerState();

    void set_affinity(long long user_id, long long channel_id, std::chrono::system_clock::time_point expires_at);
    std::optional<long long> get_affinity(long long user_id, std::chrono::system_clock::time_point now);

    void record_rpm(std::string_view credential_key, std::chrono::system_clock::time_point when);
    int rpm(std::string_view credential_key, std::chrono::system_clock::time_point now,
            std::chrono::milliseconds window) const;

    void set_credential_cooldown(std::string_view credential_key, std::chrono::system_clock::time_point until);
    bool is_credential_cooling(std::string_view credential_key, std::chrono::system_clock::time_point now) const;

    void set_channel_route_cooldown(long long channel_id, std::chrono::system_clock::time_point until);
    bool is_channel_route_cooling(long long channel_id, std::chrono::system_clock::time_point now) const;
    void clear_channel_route_cooldown(long long channel_id);

    void set_model_cooldown(long long model_binding_id, std::chrono::system_clock::time_point until);
    bool is_model_cooling(long long model_binding_id, std::chrono::system_clock::time_point now) const;
    void clear_model_cooldown(long long model_binding_id);

    void record_channel_failure(long long channel_id);
    int channel_fail_score(long long channel_id) const;
    void reset_channel_fail_score(long long channel_id);

    bool is_channel_banned(long long channel_id, std::chrono::system_clock::time_point now) const;
    std::optional<SchedulerBanEntry> channel_ban(long long channel_id, std::chrono::system_clock::time_point now) const;
    std::chrono::system_clock::time_point ban_channel(long long channel_id, std::chrono::system_clock::time_point now,
                                                      std::chrono::milliseconds base);
    std::chrono::system_clock::time_point ban_channel_immediate(long long channel_id,
                                                                std::chrono::system_clock::time_point now,
                                                                std::chrono::milliseconds base);
    void clear_channel_ban(long long channel_id);

    bool is_channel_probe_due(long long channel_id) const;
    bool is_channel_probe_pending(long long channel_id, std::chrono::system_clock::time_point now) const;
    bool try_claim_channel_probe(long long channel_id, std::chrono::system_clock::time_point now,
                                 std::chrono::milliseconds ttl);
    void release_channel_probe_claim(long long channel_id);
    void clear_channel_probe(long long channel_id);
    void sweep_expired_channel_bans(std::chrono::system_clock::time_point now);

    void attach_redis_state_store(std::shared_ptr<SchedulerRedisStateStore> redis_state_store);
    void load_redis_state();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Scheduler {
public:
    explicit Scheduler(SchedulerRoutingDataSource &data_source);

    SchedulerState &state()
    {
        return state_;
    }
    const SchedulerState &state() const
    {
        return state_;
    }

    void rebuild_routing_snapshot();
    const SchedulerRoutingSnapshot *routing_snapshot() const;
    std::uint64_t routing_generation() const;

    std::string route_key_hash(std::string_view route_key) const;

    SchedulerSelection select(long long user_id, std::string_view route_key_hash,
                              const SchedulerConstraints &constraints = {});
    void report(const SchedulerSelection &selection, const SchedulerResult &result);

    void set_affinity_ttl(std::chrono::minutes ttl);
    void set_rpm_window(std::chrono::milliseconds window);
    void set_cooldown_base(std::chrono::milliseconds cooldown);
    void set_probe_claim_ttl(std::chrono::milliseconds ttl);

private:
    SchedulerSelection select_channel_key(const Channel &channel, std::chrono::system_clock::time_point now,
                                          const SchedulerConstraints &constraints, bool *ok) const;
    SchedulerSelection select_channel_candidate(long long user_id, const Channel &channel,
                                                std::chrono::system_clock::time_point now,
                                                std::string_view route_key_hash,
                                                const SchedulerConstraints &constraints, bool *ok);
    bool channel_matches_constraints(const Channel &channel, const SchedulerConstraints &constraints,
                                     std::chrono::system_clock::time_point now) const;

    SchedulerRoutingDataSource &data_source_;
    SchedulerState state_;
    std::shared_ptr<SchedulerRoutingSnapshot> routing_snapshot_;
    std::chrono::minutes affinity_ttl_{ 30 };
    std::chrono::milliseconds rpm_window_{ 60000 };
    std::chrono::milliseconds cooldown_base_{ 30000 };
    std::chrono::milliseconds probe_claim_ttl_{ 30000 };
};

std::uint64_t scheduler_rendezvous_score(std::string_view route_key_hash, std::string_view kind, long long id);

} // namespace revlm
