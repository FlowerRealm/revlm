#include "scheduler/scheduler.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

struct FakeRedisStateStore final : revlm::SchedulerRedisStateStore {
    std::unordered_map<std::string, std::string> values;

    std::vector<std::string> scan_prefix(std::string_view prefix) override
    {
        std::vector<std::string> keys;
        for (const auto &[key, _] : values) {
            if (key.starts_with(prefix)) {
                keys.push_back(key);
            }
        }
        return keys;
    }

    std::optional<std::string> get(std::string_view key) override
    {
        const auto it = values.find(std::string{ key });
        return it == values.end() ? std::nullopt : std::optional<std::string>{ it->second };
    }

    void set(std::string_view key, std::string_view value) override
    {
        values[std::string{ key }] = std::string{ value };
    }

    void del(std::string_view key) override
    {
        values.erase(std::string{ key });
    }
};

struct FakeRoutingDataSource final : revlm::SchedulerRoutingDataSource {
    std::vector<revlm::Channel> channels;
    std::vector<revlm::ChannelGroup> groups;

    std::vector<revlm::Channel> list_channels() override
    {
        return channels;
    }
    std::vector<revlm::ChannelGroup> list_channel_groups() override
    {
        return groups;
    }
};

revlm::Channel make_channel(long long id, int type, std::string_view name, int status = 1, int priority = 0,
                            std::string_view base_url = "https://example.com", std::string_view api_key = "sk-test")
{
    revlm::Channel channel;
    channel.id = id;
    channel.type = type;
    channel.name = std::string{ name };
    channel.status = status;
    channel.priority = priority;
    channel.base_url = std::string{ base_url };
    channel.api_key = std::string{ api_key };
    return channel;
}

revlm::ChannelGroup make_group(long long id, std::string_view name, std::vector<revlm::Channel> channels = {},
                               int pointer = 0, double price_multiplier = 1.0)
{
    revlm::ChannelGroup group;
    group.id = id;
    group.name = std::string{ name };
    group.price_multiplier = price_multiplier;
    group.status = 1;
    group.channels = std::move(channels);
    group.pointer = pointer;
    return group;
}

revlm::SchedulerConstraints make_constraints()
{
    return revlm::SchedulerConstraints{};
}

revlm::SchedulerResult make_result()
{
    return revlm::SchedulerResult{};
}

FakeRoutingDataSource make_basic_source()
{
    FakeRoutingDataSource source;
    source.channels = {
        make_channel(1, 2, "primary", 1, 0, "https://a.example", "sk-primary"),
        make_channel(2, 2, "promo", 1, 100, "https://b.example", "sk-promo"),
        make_channel(4, 4, "claude", 1, 5, "https://claude.example", "sk-claude"),
    };
    source.groups = {
        make_group(1, "default", { source.channels[0], source.channels[1] }),
        make_group(3, "anthropic", { source.channels[2] }),
    };
    return source;
}

} // namespace

int main()
{
    {
        FakeRoutingDataSource source = make_basic_source();
        revlm::Scheduler scheduler(source);
        const auto selection = scheduler.select(10, "", {});
        if (expect(selection.channel_id == 2, "higher priority channel should be selected") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source = make_basic_source();
        revlm::Scheduler scheduler(source);
        scheduler.state().set_affinity(99, 1, std::chrono::system_clock::now() + 10min);
        const auto selection = scheduler.select(99, "", {});
        if (expect(selection.channel_id == 2,
                   "higher priority should still beat affinity when better candidate exists") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "a", 1, 0, "https://1.example", "sk-a"),
            make_channel(2, 2, "b", 1, 0, "https://2.example", "sk-b"),
        };
        source.groups = { make_group(1, "default", source.channels) };
        revlm::Scheduler scheduler(source);
        const std::string route_key_hash = scheduler.route_key_hash("session_abc");
        const auto first = scheduler.select(1, route_key_hash, {});
        const auto second = scheduler.select(1, route_key_hash, {});
        if (expect(first.channel_id == second.channel_id && first.api_key == second.api_key,
                   "route key should keep channel sticky") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source = make_basic_source();
        revlm::Scheduler scheduler(source);
        auto constraints = make_constraints();
        constraints.required_api = revlm::SchedulerApi::anthropic;
        constraints.requested_model = "claude-opus-4-8";
        const auto selection = scheduler.select(5, "", constraints);
        if (expect(selection.channel_id == 4 && selection.api_key == "sk-claude",
                   "anthropic api plus model should select anthropic channel only") != 0 ||
            expect(selection.model_binding_id > 0, "requested model should resolve binding id") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source = make_basic_source();
        revlm::Scheduler scheduler(source);
        scheduler.state().set_credential_cooldown("channel:2", std::chrono::system_clock::now() + 10min);
        try {
            auto constraints = make_constraints();
            constraints.required_channel_id = 2;
            (void)scheduler.select(5, "", constraints);
            if (expect(false, "required cooling channel should reject selection") != 0) {
                return 1;
            }
        } catch (const std::runtime_error &) {
        }
    }

    {
        FakeRoutingDataSource source = make_basic_source();
        revlm::Scheduler scheduler(source);
        scheduler.state().ban_channel(2, std::chrono::system_clock::now(), 1min);
        scheduler.state().ban_channel(2, std::chrono::system_clock::now(), 1min);
        const auto selection = scheduler.select(1, "", {});
        if (expect(selection.channel_id != 2, "banned channel should not be selected") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source = make_basic_source();
        revlm::Scheduler scheduler(source);
        auto warmup = make_constraints();
        warmup.required_channel_id = 1;
        warmup.requested_model = "gpt-5.5";
        const auto channel_one = scheduler.select(1, "", warmup);
        scheduler.state().set_model_cooldown(channel_one.model_binding_id, std::chrono::system_clock::now() + 5min);
        auto constraints = make_constraints();
        constraints.allowed_groups = { "default" };
        constraints.requested_model = "gpt-5.5";
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id == 2, "model-scoped cooldown should move selection away from cooled binding") !=
            0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source = make_basic_source();
        revlm::Scheduler scheduler(source);
        scheduler.set_cooldown_base(100ms);
        auto constraints = make_constraints();
        constraints.required_channel_id = 1;
        const auto selection = scheduler.select(1, "", constraints);
        auto result = make_result();
        result.success = false;
        result.retriable = true;
        result.status_code = 429;
        result.failure_scope = revlm::SchedulerFailureScope::credential;
        scheduler.report(selection, result);
        try {
            auto retry_constraints = make_constraints();
            retry_constraints.required_channel_id = selection.channel_id;
            (void)scheduler.select(1, "", retry_constraints);
            if (expect(false, "credential-scoped failure should cool that channel") != 0) {
                return 1;
            }
        } catch (const std::runtime_error &) {
        }
    }

    {
        FakeRoutingDataSource source = make_basic_source();
        revlm::Scheduler scheduler(source);
        scheduler.set_cooldown_base(100ms);
        auto constraints = make_constraints();
        constraints.required_channel_id = 2;
        constraints.requested_model = "gpt-5.5";
        const auto selection = scheduler.select(1, "", constraints);
        auto result = make_result();
        result.success = false;
        result.retriable = true;
        result.status_code = 503;
        result.error_class = "network";
        result.failure_scope = revlm::SchedulerFailureScope::channel;
        scheduler.report(selection, result);
        try {
            auto retry_constraints = make_constraints();
            retry_constraints.required_channel_id = 2;
            (void)scheduler.select(1, "", retry_constraints);
            if (expect(false, "channel-scoped failure should ban channel") != 0) {
                return 1;
            }
        } catch (const std::runtime_error &) {
        }
    }

    {
        revlm::SchedulerState state;
        auto redis = std::make_shared<FakeRedisStateStore>();
        state.attach_redis_state_store(redis);
        state.ban_channel(7, std::chrono::system_clock::now(), 1min);
        state.ban_channel(7, std::chrono::system_clock::now(), 1min);

        revlm::SchedulerState restored;
        restored.attach_redis_state_store(redis);
        restored.load_redis_state();
        if (expect(restored.is_channel_banned(7, std::chrono::system_clock::now()),
                   "redis state should restore channel bans") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source = make_basic_source();
        revlm::Scheduler scheduler(source);
        auto constraints = make_constraints();
        constraints.allowed_group_order = { "anthropic", "default" };
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id == 4, "ordered groups should prefer first enabled group") != 0 ||
            expect(selection.route_group == "anthropic", "ordered group selection should carry route group name") !=
                0 ||
            expect(selection.route_group_multiplier == 1.0, "route group should expose group multiplier") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", 1, 0, "https://1.example", "sk-1"),
            make_channel(2, 2, "second", 1, 0, "https://2.example", "sk-2"),
            make_channel(3, 2, "third", 1, 0, "https://3.example", "sk-3"),
        };
        source.groups = {
            make_group(1, "g1", { source.channels[0], source.channels[1] }),
            make_group(2, "g2", { source.channels[2] }),
        };

        revlm::Scheduler scheduler(source);
        auto constraints = make_constraints();
        constraints.allowed_group_order = { "g1", "g2" };
        constraints.sequential_channel_failover = true;
        constraints.start_channel_id = 1;
        constraints.start_channel_exclusive = true;
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id == 2, "sequential failover should start after exclusive pinned channel") != 0) {
            return 1;
        }

        scheduler.state().ban_channel(2, std::chrono::system_clock::now(), 1min);
        scheduler.state().ban_channel(2, std::chrono::system_clock::now(), 1min);
        auto retry = make_constraints();
        retry.allowed_group_order = { "g1", "g2" };
        retry.sequential_channel_failover = true;
        retry.start_channel_id = 2;
        const auto after_ban = scheduler.select(1, "", retry);
        if (expect(after_ban.channel_id == 3, "sequential failover should skip banned cached candidate and continue") !=
            0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", 0, 100, "https://1.example", "sk-1"),
            make_channel(2, 2, "second", 1, 0, "https://2.example", "sk-2"),
            make_channel(3, 2, "third", 1, 0, "https://3.example", "sk-3"),
        };
        source.groups = { make_group(1, "default", source.channels, 0) };

        revlm::Scheduler scheduler(source);
        auto constraints = make_constraints();
        constraints.allowed_group_order = { "default" };
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id == 2,
                   "group pointer should fall through disabled candidate to next live channel") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", 1, 0, "https://1.example", "sk-1"),
            make_channel(2, 2, "second", 1, 0, "https://2.example", "sk-2"),
            make_channel(3, 2, "third", 1, 0, "https://3.example", "sk-3"),
        };
        source.groups = { make_group(1, "default", source.channels, 0) };

        revlm::Scheduler scheduler(source);
        scheduler.state().ban_channel(1, std::chrono::system_clock::now(), 1min);
        scheduler.state().ban_channel(1, std::chrono::system_clock::now(), 1min);
        auto constraints = make_constraints();
        constraints.allowed_group_order = { "default" };
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id == 2,
                   "group pointer should rotate past banned channel to next live candidate") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", 1, 0, "https://1.example", "sk-1"),
            make_channel(2, 2, "second", 1, 0, "https://2.example", "sk-2"),
        };
        source.groups = { make_group(1, "premium", source.channels, 0, 1.5) };

        revlm::Scheduler scheduler(source);
        auto constraints = make_constraints();
        constraints.allowed_group_order = { "premium" };
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.route_group == "premium", "ordered group should set route group name") != 0 ||
            expect(selection.route_group_multiplier == 1.5, "route group should expose configured price multiplier") !=
                0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", 1, 0, "https://1.example", "sk-1"),
            make_channel(2, 2, "second", 1, 0, "https://2.example", "sk-2"),
        };
        source.groups = {
            make_group(1, "g1", { source.channels[0] }),
            make_group(2, "g2", { source.channels[1] }),
        };

        revlm::Scheduler scheduler(source);
        scheduler.state().ban_channel(1, std::chrono::system_clock::now(), 1min);
        scheduler.state().ban_channel(1, std::chrono::system_clock::now(), 1min);
        auto constraints = make_constraints();
        constraints.allowed_group_order = { "g1", "g2" };
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id == 2,
                   "ordered groups should fall through exhausted first group to second group") != 0 ||
            expect(selection.route_group == "g2", "fallback group should carry its route group name") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", 1, 0, "https://1.example", "sk-1"),
            make_channel(2, 2, "second", 1, 0, "https://2.example", "sk-2"),
        };
        source.groups = { make_group(1, "default", source.channels) };

        revlm::Scheduler scheduler(source);
        scheduler.state().record_channel_failure(1);
        scheduler.state().record_channel_failure(1);
        auto constraints = make_constraints();
        constraints.allowed_group_order = { "default" };
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id == 2,
                   "group router should prefer lower fail-score channel within same group") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", 1, 0, "https://1.example", "sk-1"),
            make_channel(2, 2, "second", 1, 0, "https://2.example", "sk-2"),
        };
        source.groups = { make_group(1, "default", source.channels) };

        revlm::Scheduler scheduler(source);
        auto constraints = make_constraints();
        constraints.allowed_group_order = { "default" };
        constraints.soft_excluded_channel_ids.insert(1);
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id == 2,
                   "soft exclude should skip channel until fallback retry without soft excludes") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source = make_basic_source();
        revlm::Scheduler scheduler(source);
        scheduler.set_cooldown_base(100ms);
        auto constraints = make_constraints();
        constraints.required_channel_id = 1;
        const auto selection = scheduler.select(1, "", constraints);
        auto result = make_result();
        result.success = false;
        result.retriable = true;
        result.status_code = 503;
        scheduler.state().set_channel_route_cooldown(1, std::chrono::system_clock::now() + 10min);
        try {
            auto retry_constraints = make_constraints();
            retry_constraints.required_channel_id = 1;
            (void)scheduler.select(1, "", retry_constraints);
            if (expect(false, "channel route cooldown should reject selection") != 0) {
                return 1;
            }
        } catch (const std::runtime_error &) {
        }
    }

    {
        FakeRoutingDataSource source = make_basic_source();
        revlm::Scheduler scheduler(source);
        scheduler.set_cooldown_base(100ms);
        auto constraints = make_constraints();
        constraints.required_channel_id = 2;
        constraints.requested_model = "gpt-5.5";
        const auto selection = scheduler.select(1, "", constraints);
        auto result = make_result();
        result.success = false;
        result.retriable = true;
        result.status_code = 404;
        result.failure_scope = revlm::SchedulerFailureScope::model;
        scheduler.report(selection, result);
        try {
            auto retry_constraints = make_constraints();
            retry_constraints.allowed_groups = { "default" };
            retry_constraints.requested_model = "gpt-5.5";
            const auto retry = scheduler.select(1, "", retry_constraints);
            if (expect(retry.channel_id != 2,
                       "model-scoped failure should cool binding and move away from channel 2") != 0) {
                return 1;
            }
        } catch (const std::runtime_error &) {
            if (expect(false, "model-scoped failure should leave another reachable candidate") != 0) {
                return 1;
            }
        }
    }

    return 0;
}
