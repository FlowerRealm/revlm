#include "scheduler/scheduler.hpp"
#include "models/models.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <openssl/sha.h>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;
using Ms = std::chrono::milliseconds;

constexpr std::string_view redis_ban_prefix = "revlm:scheduler:ban:";
constexpr std::string_view redis_channel_ban_prefix = "revlm:scheduler:ban:channel:";
constexpr std::string_view redis_model_ban_prefix = "revlm:scheduler:ban:model:";

constexpr std::string_view scheduler_type_openai = "openai_compatible";
constexpr std::string_view scheduler_type_anthropic = "anthropic";

bool channel_type_is_openai(int type)
{
    return type == 1 || type == 2;
}

bool channel_type_is_anthropic(int type)
{
    return type == 4;
}

std::string channel_scheduler_type_name(int type)
{
    if (channel_type_is_anthropic(type)) {
        return std::string{ scheduler_type_anthropic };
    }
    if (channel_type_is_openai(type)) {
        return std::string{ scheduler_type_openai };
    }
    return {};
}

bool api_supports_channel_type(SchedulerApi api, int channel_type)
{
    if (api == SchedulerApi::anthropic) {
        return channel_type_is_anthropic(channel_type);
    }
    return channel_type_is_openai(channel_type);
}

std::uint64_t fnv1a64(std::string_view value, std::uint64_t seed = 14695981039346656037ULL)
{
    std::uint64_t hash = seed;
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

template <typename T> void hash_number(std::uint64_t *hash, T value)
{
    const std::uint64_t raw = static_cast<std::uint64_t>(value);
    for (size_t i = 0; i < sizeof(raw); ++i) {
        *hash ^= static_cast<unsigned char>((raw >> (i * 8U)) & 0xffU);
        *hash *= 1099511628211ULL;
    }
}

std::uint64_t snapshot_generation(const SchedulerRoutingSnapshot &snapshot)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const Channel &channel : snapshot.channels) {
        hash = fnv1a64(channel.name, hash);
        hash = fnv1a64(channel.base_url, hash);
        hash_number(&hash, channel.id);
        hash_number(&hash, channel.type);
        hash_number(&hash, channel.status ? 1 : 0);
        hash_number(&hash, channel.priority);
        hash_number(&hash, channel.price_multiplier);
        hash = fnv1a64(channel.api_key, hash);
    }
    return hash == 0 ? 1 : hash;
}

long long channel_model_binding_id(std::string_view model_name, long long channel_id)
{
    const std::vector<Model> &models = ModelManager::instance().models();
    const auto model_it = std::ranges::find(models, model_name, &Model::name);
    if (model_it == models.end() || channel_id <= 0) {
        return 0;
    }
    std::uint64_t hash = fnv1a64(std::to_string(model_it->id));
    hash_number(&hash, static_cast<std::uint64_t>(channel_id));
    const long long id = static_cast<long long>(hash & 0x3fffffffffffffffULL);
    return id == 0 ? channel_id : id;
}

std::string serialize_ban_entry(const SchedulerBanEntry &entry)
{
    return std::to_string(std::chrono::duration_cast<Ms>(entry.until.time_since_epoch()).count()) + ":" +
           std::to_string(entry.streak);
}

std::optional<SchedulerBanEntry> deserialize_ban_entry(std::string_view raw)
{
    const size_t sep = raw.find(':');
    if (sep == std::string_view::npos) {
        return std::nullopt;
    }
    try {
        const long long until_ms = std::stoll(std::string{ raw.substr(0, sep) });
        const int streak = std::stoi(std::string{ raw.substr(sep + 1) });
        if (until_ms <= 0 || streak < 0) {
            return std::nullopt;
        }
        return SchedulerBanEntry{
            .until = TimePoint{ Ms{ until_ms } },
            .streak = streak,
        };
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

std::optional<long long> parse_redis_id(std::string_view key, std::string_view prefix)
{
    if (!key.starts_with(prefix)) {
        return std::nullopt;
    }
    try {
        const long long id = std::stoll(std::string{ key.substr(prefix.size()) });
        if (id <= 0) {
            return std::nullopt;
        }
        return id;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

std::string channel_ban_key(long long channel_id)
{
    return std::string{ redis_channel_ban_prefix } + std::to_string(channel_id);
}

std::string model_ban_key(long long model_binding_id)
{
    return std::string{ redis_model_ban_prefix } + std::to_string(model_binding_id);
}

bool should_ban_channel_immediately(const SchedulerResult &result)
{
    switch (result.status_code) {
    case 401:
    case 402:
    case 403:
    case 429:
        return false;
    default:
        break;
    }

    return result.error_class == "network" || result.error_class == "read_upstream" ||
           result.error_class == "stream_idle_timeout" || result.error_class == "stream_read_error" ||
           result.error_class == "stream_first_byte_timeout" ||
           ((result.error_class == "upstream_status") && (result.status_code == 404 || result.status_code == 405));
}

template <typename Item, typename ScoreFn>
void rendezvous_sort(std::vector<Item> *items, std::string_view route_key_hash, std::string_view kind, ScoreFn item_id)
{
    if (items == nullptr || items->size() <= 1 || route_key_hash.empty()) {
        return;
    }
    std::stable_sort(items->begin(), items->end(), [&](const Item &left, const Item &right) {
        const std::uint64_t left_score = scheduler_rendezvous_score(route_key_hash, kind, item_id(left));
        const std::uint64_t right_score = scheduler_rendezvous_score(route_key_hash, kind, item_id(right));
        if (left_score != right_score) {
            return left_score > right_score;
        }
        return item_id(left) > item_id(right);
    });
}

std::vector<Channel> order_channels(const std::vector<Channel> &channels, std::optional<long long> affinity_channel_id,
                                    const SchedulerState &state, TimePoint now)
{
    std::vector<Channel> probes;
    std::vector<Channel> normal;
    for (const Channel &channel : channels) {
        if (state.is_channel_probe_pending(channel.id, now)) {
            probes.push_back(channel);
        } else {
            normal.push_back(channel);
        }
    }
    auto by_priority = [&](const Channel &left, const Channel &right) {
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        const bool left_affinity = affinity_channel_id.has_value() && left.id == *affinity_channel_id;
        const bool right_affinity = affinity_channel_id.has_value() && right.id == *affinity_channel_id;
        if (left_affinity != right_affinity) {
            return left_affinity;
        }
        const int left_fail = state.channel_fail_score(left.id);
        const int right_fail = state.channel_fail_score(right.id);
        if (left_fail != right_fail) {
            return left_fail < right_fail;
        }
        return left.id > right.id;
    };
    std::stable_sort(probes.begin(), probes.end(), by_priority);
    std::stable_sort(normal.begin(), normal.end(), by_priority);

    std::vector<Channel> out;
    std::unordered_set<long long> seen;
    auto append = [&](const std::vector<Channel> &items) {
        for (const Channel &channel : items) {
            if (seen.insert(channel.id).second) {
                out.push_back(channel);
            }
        }
    };
    append(probes);
    append(normal);
    return out;
}

} // namespace

struct SchedulerState::Impl {
    mutable std::mutex mu;

    std::unordered_map<long long, std::pair<long long, TimePoint>> affinity;
    std::unordered_map<std::string, std::vector<TimePoint>> rpm_by_credential;
    std::unordered_map<std::string, TimePoint> credential_cooldowns;
    std::unordered_map<long long, TimePoint> channel_route_cooldowns;
    std::unordered_map<long long, TimePoint> model_cooldowns;
    std::unordered_map<long long, int> channel_fail_scores;
    std::unordered_map<long long, SchedulerBanEntry> channel_bans;
    std::unordered_map<long long, TimePoint> channel_probe_due_at;
    std::unordered_map<long long, TimePoint> channel_probe_claim_until;
    std::shared_ptr<SchedulerRedisStateStore> redis_state_store;
};

std::string SchedulerSelection::credential_key() const
{
    return "channel:" + std::to_string(channel_id);
}

std::uint64_t scheduler_rendezvous_score(std::string_view route_key_hash, std::string_view kind, long long id)
{
    if (route_key_hash.empty() || kind.empty() || id <= 0) {
        return 0;
    }
    const std::string key = std::string{ route_key_hash } + ":" + std::string{ kind } + ":" + std::to_string(id);
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char *>(key.data()), key.size(), digest.data());
    std::uint64_t score = 0;
    for (size_t i = 0; i < sizeof(score); ++i) {
        score = (score << 8U) | digest[i];
    }
    return score;
}

bool SchedulerRoutingSnapshot::channel_supports_model(long long channel_id, std::string_view public_id) const
{
    if (public_id.empty()) {
        return false;
    }
    const std::vector<Model> &models = ModelManager::instance().models();
    const auto model_it = std::ranges::find(models, public_id, &Model::name);
    if (model_it == models.end()) {
        return false;
    }
    const auto channel_it = std::ranges::find(channels, channel_id, &Channel::id);
    if (channel_it == channels.end() || !channel_it->status) {
        return false;
    }
    return (model_it->owned_by == "openai" && channel_type_is_openai(channel_it->type)) ||
           (model_it->owned_by == "anthropic" && channel_type_is_anthropic(channel_it->type));
}

SchedulerState::SchedulerState()
    : impl_(std::make_unique<Impl>())
{
}

SchedulerState::~SchedulerState() = default;

void SchedulerState::set_affinity(long long user_id, long long channel_id, TimePoint expires_at)
{
    if (user_id <= 0 || channel_id <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->affinity[user_id] = std::make_pair(channel_id, expires_at);
}

std::optional<long long> SchedulerState::get_affinity(long long user_id, TimePoint now)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto it = impl_->affinity.find(user_id);
    if (it == impl_->affinity.end()) {
        return std::nullopt;
    }
    if (now > it->second.second) {
        impl_->affinity.erase(it);
        return std::nullopt;
    }
    return it->second.first;
}

void SchedulerState::record_rpm(std::string_view credential_key, TimePoint when)
{
    const std::string key = trim_ascii(credential_key);
    if (key.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->rpm_by_credential[key].push_back(when);
}

int SchedulerState::rpm(std::string_view credential_key, TimePoint now, Ms window) const
{
    const std::string key = trim_ascii(credential_key);
    if (key.empty()) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto &events = impl_->rpm_by_credential[key];
    const TimePoint cutoff = now - window;
    events.erase(std::remove_if(events.begin(), events.end(), [&](const TimePoint &item) { return item < cutoff; }),
                 events.end());
    return static_cast<int>(events.size());
}

void SchedulerState::set_credential_cooldown(std::string_view credential_key, TimePoint until)
{
    const std::string key = trim_ascii(credential_key);
    if (key.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->credential_cooldowns[key] = until;
}

bool SchedulerState::is_credential_cooling(std::string_view credential_key, TimePoint now) const
{
    const std::string key = trim_ascii(credential_key);
    if (key.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto it = impl_->credential_cooldowns.find(key);
    if (it == impl_->credential_cooldowns.end()) {
        return false;
    }
    if (now > it->second) {
        impl_->credential_cooldowns.erase(it);
        return false;
    }
    return true;
}

void SchedulerState::set_channel_route_cooldown(long long channel_id, TimePoint until)
{
    if (channel_id <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->channel_route_cooldowns[channel_id] = until;
}

bool SchedulerState::is_channel_route_cooling(long long channel_id, TimePoint now) const
{
    if (channel_id <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto it = impl_->channel_route_cooldowns.find(channel_id);
    if (it == impl_->channel_route_cooldowns.end()) {
        return false;
    }
    if (now > it->second) {
        impl_->channel_route_cooldowns.erase(it);
        return false;
    }
    return true;
}

void SchedulerState::clear_channel_route_cooldown(long long channel_id)
{
    if (channel_id <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->channel_route_cooldowns.erase(channel_id);
}

void SchedulerState::set_model_cooldown(long long model_binding_id, TimePoint until)
{
    if (model_binding_id <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->model_cooldowns[model_binding_id] = until;
    if (impl_->redis_state_store != nullptr) {
        impl_->redis_state_store->set(model_ban_key(model_binding_id),
                                      serialize_ban_entry({ .until = until, .streak = 1 }));
    }
}

bool SchedulerState::is_model_cooling(long long model_binding_id, TimePoint now) const
{
    if (model_binding_id <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto it = impl_->model_cooldowns.find(model_binding_id);
    if (it == impl_->model_cooldowns.end()) {
        return false;
    }
    if (now > it->second) {
        if (impl_->redis_state_store != nullptr) {
            impl_->redis_state_store->del(model_ban_key(model_binding_id));
        }
        impl_->model_cooldowns.erase(it);
        return false;
    }
    return true;
}

void SchedulerState::clear_model_cooldown(long long model_binding_id)
{
    if (model_binding_id <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->model_cooldowns.erase(model_binding_id);
    if (impl_->redis_state_store != nullptr) {
        impl_->redis_state_store->del(model_ban_key(model_binding_id));
    }
}

void SchedulerState::record_channel_failure(long long channel_id)
{
    if (channel_id <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->channel_fail_scores[channel_id] += 1;
}

int SchedulerState::channel_fail_score(long long channel_id) const
{
    if (channel_id <= 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto it = impl_->channel_fail_scores.find(channel_id);
    return it == impl_->channel_fail_scores.end() ? 0 : it->second;
}

void SchedulerState::reset_channel_fail_score(long long channel_id)
{
    if (channel_id <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->channel_fail_scores.erase(channel_id);
}

bool SchedulerState::is_channel_banned(long long channel_id, TimePoint now) const
{
    return channel_ban(channel_id, now).has_value();
}

std::optional<SchedulerBanEntry> SchedulerState::channel_ban(long long channel_id, TimePoint now) const
{
    if (channel_id <= 0) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto it = impl_->channel_bans.find(channel_id);
    if (it == impl_->channel_bans.end()) {
        return std::nullopt;
    }
    if (now > it->second.until) {
        impl_->channel_bans.erase(it);
        impl_->channel_probe_due_at[channel_id] = now;
        impl_->channel_probe_claim_until.erase(channel_id);
        if (impl_->redis_state_store != nullptr) {
            impl_->redis_state_store->del(channel_ban_key(channel_id));
        }
        return std::nullopt;
    }
    return it->second;
}

TimePoint SchedulerState::ban_channel(long long channel_id, TimePoint now, Ms base)
{
    if (channel_id <= 0 || base.count() <= 0) {
        return now;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    SchedulerBanEntry &entry = impl_->channel_bans[channel_id];
    entry.streak = std::min(entry.streak + 1, 20);
    if (entry.streak < 2) {
        return now;
    }
    TimePoint start = entry.until > now ? entry.until : now;
    entry.until = std::min(start + (base * entry.streak), now + std::chrono::minutes(10));
    if (impl_->redis_state_store != nullptr) {
        impl_->redis_state_store->set(channel_ban_key(channel_id), serialize_ban_entry(entry));
    }
    return entry.until;
}

TimePoint SchedulerState::ban_channel_immediate(long long channel_id, TimePoint now, Ms base)
{
    if (channel_id <= 0 || base.count() <= 0) {
        return now;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    SchedulerBanEntry &entry = impl_->channel_bans[channel_id];
    entry.streak = std::min(entry.streak + 1, 20);
    TimePoint start = entry.until > now ? entry.until : now;
    entry.until = std::min(start + (base * entry.streak), now + std::chrono::minutes(10));
    if (impl_->redis_state_store != nullptr) {
        impl_->redis_state_store->set(channel_ban_key(channel_id), serialize_ban_entry(entry));
    }
    return entry.until;
}

void SchedulerState::clear_channel_ban(long long channel_id)
{
    if (channel_id <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->channel_bans.erase(channel_id);
    impl_->channel_probe_due_at.erase(channel_id);
    impl_->channel_probe_claim_until.erase(channel_id);
    if (impl_->redis_state_store != nullptr) {
        impl_->redis_state_store->del(channel_ban_key(channel_id));
    }
}

bool SchedulerState::is_channel_probe_due(long long channel_id) const
{
    if (channel_id <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->channel_probe_due_at.contains(channel_id);
}

bool SchedulerState::is_channel_probe_pending(long long channel_id, TimePoint now) const
{
    if (channel_id <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->channel_probe_due_at.contains(channel_id)) {
        return false;
    }
    const auto it = impl_->channel_probe_claim_until.find(channel_id);
    if (it == impl_->channel_probe_claim_until.end()) {
        return true;
    }
    if (now > it->second) {
        impl_->channel_probe_claim_until.erase(it);
        return true;
    }
    return false;
}

bool SchedulerState::try_claim_channel_probe(long long channel_id, TimePoint now, Ms ttl)
{
    if (channel_id <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->channel_probe_due_at.contains(channel_id)) {
        return false;
    }
    const auto it = impl_->channel_probe_claim_until.find(channel_id);
    if (it != impl_->channel_probe_claim_until.end() && now <= it->second) {
        return false;
    }
    impl_->channel_probe_claim_until[channel_id] = now + ttl;
    return true;
}

void SchedulerState::release_channel_probe_claim(long long channel_id)
{
    if (channel_id <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->channel_probe_claim_until.erase(channel_id);
}

void SchedulerState::clear_channel_probe(long long channel_id)
{
    if (channel_id <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->channel_probe_due_at.erase(channel_id);
    impl_->channel_probe_claim_until.erase(channel_id);
}

void SchedulerState::sweep_expired_channel_bans(TimePoint now)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::vector<long long> expired;
    for (const auto &[channel_id, entry] : impl_->channel_bans) {
        if (now > entry.until) {
            expired.push_back(channel_id);
        }
    }
    for (long long channel_id : expired) {
        impl_->channel_bans.erase(channel_id);
        impl_->channel_probe_due_at[channel_id] = now;
        impl_->channel_probe_claim_until.erase(channel_id);
        if (impl_->redis_state_store != nullptr) {
            impl_->redis_state_store->del(channel_ban_key(channel_id));
        }
    }
}

void SchedulerState::attach_redis_state_store(std::shared_ptr<SchedulerRedisStateStore> redis_state_store)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->redis_state_store = std::move(redis_state_store);
}

void SchedulerState::load_redis_state()
{
    std::shared_ptr<SchedulerRedisStateStore> redis_state_store;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        redis_state_store = impl_->redis_state_store;
    }
    if (redis_state_store == nullptr) {
        return;
    }
    const TimePoint now = Clock::now();
    for (const std::string &key : redis_state_store->scan_prefix(redis_ban_prefix)) {
        const std::optional<std::string> raw = redis_state_store->get(key);
        if (!raw.has_value()) {
            continue;
        }
        const auto entry = deserialize_ban_entry(*raw);
        if (!entry.has_value() || entry->until <= now) {
            redis_state_store->del(key);
            continue;
        }
        if (const auto channel_id = parse_redis_id(key, redis_channel_ban_prefix); channel_id.has_value()) {
            std::lock_guard<std::mutex> lock(impl_->mu);
            impl_->channel_bans[*channel_id] = *entry;
            continue;
        }
        if (const auto model_binding_id = parse_redis_id(key, redis_model_ban_prefix); model_binding_id.has_value()) {
            std::lock_guard<std::mutex> lock(impl_->mu);
            impl_->model_cooldowns[*model_binding_id] = entry->until;
        }
    }
}

Scheduler::Scheduler(SchedulerRoutingDataSource &data_source)
    : data_source_(data_source)
{
    rebuild_routing_snapshot();
}

void Scheduler::rebuild_routing_snapshot()
{
    auto snapshot = std::make_shared<SchedulerRoutingSnapshot>();
    snapshot->loaded_at = Clock::now();
    snapshot->channels = data_source_.list_channels();
    snapshot->generation = snapshot_generation(*snapshot);
    routing_snapshot_ = std::move(snapshot);
}

const SchedulerRoutingSnapshot *Scheduler::routing_snapshot() const
{
    return routing_snapshot_.get();
}

std::uint64_t Scheduler::routing_generation() const
{
    return routing_snapshot_ == nullptr ? 0 : routing_snapshot_->generation;
}

std::string Scheduler::route_key_hash(std::string_view route_key) const
{
    const std::string key = trim_ascii(route_key);
    if (key.empty()) {
        return {};
    }
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char *>(key.data()), key.size(), digest.data());
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        out.push_back(hex[(byte >> 4U) & 0x0fU]);
        out.push_back(hex[byte & 0x0fU]);
    }
    return out;
}

bool Scheduler::channel_matches_constraints(const Channel &channel, const SchedulerConstraints &constraints,
                                            TimePoint now) const
{
    if (!channel.status) {
        return false;
    }
    if (!channel_type_is_openai(channel.type) && !channel_type_is_anthropic(channel.type)) {
        return false;
    }
    if (trim_ascii(channel.base_url).empty() || trim_ascii(channel.api_key).empty()) {
        return false;
    }
    if (constraints.required_api.has_value() && !api_supports_channel_type(*constraints.required_api, channel.type)) {
        return false;
    }
    if (constraints.required_channel_type.has_value() &&
        trim_ascii(*constraints.required_channel_type) != channel_scheduler_type_name(channel.type)) {
        return false;
    }
    if (constraints.required_channel_id > 0 && constraints.required_channel_id != channel.id) {
        return false;
    }
    if (!constraints.allowed_channel_ids.empty() && !constraints.allowed_channel_ids.contains(channel.id)) {
        return false;
    }
    if (constraints.requested_model.has_value() &&
        (routing_snapshot_ == nullptr ||
         !routing_snapshot_->channel_supports_model(channel.id, *constraints.requested_model))) {
        return false;
    }
    if (state_.is_channel_banned(channel.id, now)) {
        return constraints.allow_banned_required_channel && constraints.required_channel_id == channel.id;
    }
    return true;
}

SchedulerSelection Scheduler::select(long long user_id, std::string_view route_key_hash_value,
                                     const SchedulerConstraints &constraints)
{
    if (routing_snapshot_ == nullptr) {
        rebuild_routing_snapshot();
    }
    if (routing_snapshot_ == nullptr) {
        throw std::runtime_error("routing snapshot unavailable");
    }

    const TimePoint now = Clock::now();
    state_.sweep_expired_channel_bans(now);
    std::vector<Channel> candidates;
    for (const Channel &channel : routing_snapshot_->channels) {
        if (channel_matches_constraints(channel, constraints, now)) {
            candidates.push_back(channel);
        }
    }
    if (candidates.empty()) {
        throw std::runtime_error("no enabled routing candidates");
    }

    std::optional<long long> affinity_channel = state_.get_affinity(user_id, now);
    if (affinity_channel.has_value() && state_.channel_fail_score(*affinity_channel) > 0) {
        affinity_channel.reset();
    }
    std::vector<Channel> ordered = order_channels(candidates, affinity_channel, state_, now);
    rendezvous_sort(&ordered, route_key_hash_value, "channel", [](const Channel &channel) { return channel.id; });

    for (const Channel &channel : ordered) {
        bool ok = false;
        SchedulerSelection selection =
            select_channel_candidate(user_id, channel, now, route_key_hash_value, constraints, &ok);
        if (ok) {
            return selection;
        }
    }
    throw std::runtime_error("no reachable endpoint credential candidate");
}

SchedulerSelection Scheduler::select_channel_key(const Channel &channel, TimePoint now,
                                                 const SchedulerConstraints &constraints, bool *ok) const
{
    if (ok != nullptr) {
        *ok = false;
    }
    if (trim_ascii(channel.api_key).empty()) {
        return {};
    }
    if (state_.is_credential_cooling("channel:" + std::to_string(channel.id), now)) {
        return {};
    }

    SchedulerSelection selection{};
    selection.channel_id = channel.id;
    selection.channel_type = channel_scheduler_type_name(channel.type);
    selection.route_group_multiplier = channel.price_multiplier;
    selection.base_url = channel.base_url;
    selection.api_key = channel.api_key;
    if (constraints.requested_model.has_value()) {
        selection.model_public_id = *constraints.requested_model;
        selection.model_binding_id = channel_model_binding_id(selection.model_public_id, selection.channel_id);
    }
    if (selection.model_binding_id > 0 && state_.is_model_cooling(selection.model_binding_id, now)) {
        return {};
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return selection;
}

SchedulerSelection Scheduler::select_channel_candidate(long long user_id, const Channel &channel, TimePoint now,
                                                       std::string_view route_key_hash_value,
                                                       const SchedulerConstraints &constraints, bool *ok)
{
    if (ok != nullptr) {
        *ok = false;
    }
    bool claimed_probe = false;
    if (state_.is_channel_probe_due(channel.id)) {
        if (!state_.try_claim_channel_probe(channel.id, now, probe_claim_ttl_)) {
            return {};
        }
        claimed_probe = true;
    }

    if (state_.is_channel_route_cooling(channel.id, now)) {
        if (claimed_probe) {
            state_.release_channel_probe_claim(channel.id);
        }
        return {};
    }

    bool channel_ok = false;
    SchedulerSelection selection = select_channel_key(channel, now, constraints, &channel_ok);
    if (!channel_ok) {
        if (claimed_probe) {
            state_.release_channel_probe_claim(channel.id);
        }
        return {};
    }
    state_.record_rpm(selection.credential_key(), now);
    if (route_key_hash_value.empty()) {
        state_.set_affinity(user_id, channel.id, now + affinity_ttl_);
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return selection;
}

void Scheduler::report(const SchedulerSelection &selection, const SchedulerResult &result)
{
    const TimePoint now = Clock::now();
    state_.clear_channel_probe(selection.channel_id);
    if (result.success) {
        state_.clear_channel_ban(selection.channel_id);
        state_.reset_channel_fail_score(selection.channel_id);
        state_.clear_channel_route_cooldown(selection.channel_id);
        if (selection.model_binding_id > 0) {
            state_.clear_model_cooldown(selection.model_binding_id);
        }
        return;
    }

    state_.record_channel_failure(selection.channel_id);
    if (!result.retriable) {
        return;
    }

    Ms cooldown = cooldown_base_;
    if (result.status_code == 429) {
        cooldown *= 2;
    }
    TimePoint cooldown_until = now + cooldown;
    if (result.cooldown_until.has_value() && *result.cooldown_until > cooldown_until) {
        cooldown_until = *result.cooldown_until;
    }

    switch (result.failure_scope) {
    case SchedulerFailureScope::credential:
        state_.set_credential_cooldown(selection.credential_key(), cooldown_until);
        break;
    case SchedulerFailureScope::channel:
        state_.set_channel_route_cooldown(selection.channel_id, cooldown_until);
        if (should_ban_channel_immediately(result)) {
            state_.ban_channel_immediate(selection.channel_id, now, cooldown);
        } else {
            state_.ban_channel(selection.channel_id, now, cooldown);
        }
        return;
    case SchedulerFailureScope::model:
        if (selection.model_binding_id > 0) {
            state_.set_model_cooldown(selection.model_binding_id, cooldown_until);
        }
        break;
    }
}

void Scheduler::set_affinity_ttl(std::chrono::minutes ttl)
{
    affinity_ttl_ = ttl;
}

void Scheduler::set_rpm_window(Ms window)
{
    rpm_window_ = window;
}

void Scheduler::set_cooldown_base(Ms cooldown)
{
    cooldown_base_ = cooldown;
}

void Scheduler::set_probe_claim_ttl(Ms ttl)
{
    probe_claim_ttl_ = ttl;
}

} // namespace revlm
