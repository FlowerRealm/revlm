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

    std::vector<revlm::Channel> list_channels() override
    {
        return channels;
    }
};

revlm::Channel make_channel(long long id, int type, std::string_view name, bool status = true, int priority = 0,
                            std::string_view base_url = "https://example.com", std::string_view api_key = "sk-test",
                            double price_multiplier = 1.0)
{
    revlm::Channel channel;
    channel.id = id;
    channel.type = type;
    channel.name = std::string{ name };
    channel.status = status;
    channel.priority = priority;
    channel.base_url = std::string{ base_url };
    channel.api_key = std::string{ api_key };
    channel.price_multiplier = price_multiplier;
    return channel;
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
        make_channel(1, 2, "primary", true, 0, "https://a.example", "sk-primary"),
        make_channel(2, 2, "promo", true, 100, "https://b.example", "sk-promo"),
        make_channel(4, 4, "claude", true, 5, "https://claude.example", "sk-claude"),
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
            make_channel(1, 2, "a", true, 0, "https://1.example", "sk-a"),
            make_channel(2, 2, "b", true, 0, "https://2.example", "sk-b"),
        };
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
        constraints.allowed_channel_ids = { 1, 2 };
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
        constraints.required_channel_id = 4;
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id == 4, "required_channel_id should pin selection") != 0 ||
            expect(selection.route_group_multiplier == 1.0, "channel should expose default price multiplier") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", true, 0, "https://1.example", "sk-1"),
            make_channel(2, 2, "second", true, 0, "https://2.example", "sk-2"),
            make_channel(3, 2, "third", true, 0, "https://3.example", "sk-3"),
        };

        revlm::Scheduler scheduler(source);
        auto constraints = make_constraints();
        constraints.allowed_channel_ids = { 1, 2, 3 };
        constraints.soft_excluded_channel_ids.insert(1);
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id != 1, "soft exclude should skip the excluded channel") != 0) {
            return 1;
        }

        scheduler.state().ban_channel(selection.channel_id, std::chrono::system_clock::now(), 1min);
        scheduler.state().ban_channel(selection.channel_id, std::chrono::system_clock::now(), 1min);
        auto retry = make_constraints();
        retry.allowed_channel_ids = { 1, 2, 3 };
        const auto after_ban = scheduler.select(1, "", retry);
        if (expect(after_ban.channel_id != selection.channel_id,
                   "allowed set should skip banned candidate and continue") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", false, 100, "https://1.example", "sk-1"),
            make_channel(2, 2, "second", true, 0, "https://2.example", "sk-2"),
            make_channel(3, 2, "third", true, 0, "https://3.example", "sk-3"),
        };

        revlm::Scheduler scheduler(source);
        auto constraints = make_constraints();
        constraints.allowed_channel_ids = { 1, 2, 3 };
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id == 2 || selection.channel_id == 3,
                   "allowed set should fall through disabled candidate to next live channel") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", true, 0, "https://1.example", "sk-1"),
            make_channel(2, 2, "second", true, 0, "https://2.example", "sk-2"),
            make_channel(3, 2, "third", true, 0, "https://3.example", "sk-3"),
        };

        revlm::Scheduler scheduler(source);
        scheduler.state().ban_channel(1, std::chrono::system_clock::now(), 1min);
        scheduler.state().ban_channel(1, std::chrono::system_clock::now(), 1min);
        auto constraints = make_constraints();
        constraints.allowed_channel_ids = { 1, 2, 3 };
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id != 1, "allowed set should rotate past banned channel") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", true, 0, "https://1.example", "sk-1", 1.5),
            make_channel(2, 2, "second", true, 0, "https://2.example", "sk-2"),
        };

        revlm::Scheduler scheduler(source);
        auto constraints = make_constraints();
        constraints.required_channel_id = 1;
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id == 1, "required channel should be selected") != 0 ||
            expect(selection.route_group_multiplier == 1.5, "selection should expose channel price multiplier") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", true, 0, "https://1.example", "sk-1"),
            make_channel(2, 2, "second", true, 0, "https://2.example", "sk-2"),
        };

        revlm::Scheduler scheduler(source);
        scheduler.state().ban_channel(1, std::chrono::system_clock::now(), 1min);
        scheduler.state().ban_channel(1, std::chrono::system_clock::now(), 1min);
        auto constraints = make_constraints();
        constraints.allowed_channel_ids = { 1, 2 };
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id == 2,
                   "allowed channels should fall through banned first channel to second") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", true, 0, "https://1.example", "sk-1"),
            make_channel(2, 2, "second", true, 0, "https://2.example", "sk-2"),
        };

        revlm::Scheduler scheduler(source);
        scheduler.state().record_channel_failure(1);
        scheduler.state().record_channel_failure(1);
        auto constraints = make_constraints();
        constraints.allowed_channel_ids = { 1, 2 };
        const auto selection = scheduler.select(1, "", constraints);
        if (expect(selection.channel_id == 2, "router should prefer lower fail-score channel") != 0) {
            return 1;
        }
    }

    {
        FakeRoutingDataSource source;
        source.channels = {
            make_channel(1, 2, "first", true, 0, "https://1.example", "sk-1"),
            make_channel(2, 2, "second", true, 0, "https://2.example", "sk-2"),
        };

        revlm::Scheduler scheduler(source);
        auto constraints = make_constraints();
        constraints.allowed_channel_ids = { 1, 2 };
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
            retry_constraints.allowed_channel_ids = { 1, 2 };
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
