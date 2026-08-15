#include "store/mysql_test_env.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{

int expect(bool ok, const char *message)
{
    if (ok) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

void reset_contract_tables(odb::database &db)
{
    revlm::sql_exec(db, "DELETE FROM channel_group_members");
    revlm::sql_exec(db, "DELETE FROM channels");
    revlm::sql_exec(db, "DELETE FROM channel_groups");
}

revlm::Channel make_channel(std::string name, int priority, std::string base_url = "")
{
    return revlm::Channel(0, "openai_compatible", std::move(name), true, priority, std::move(base_url));
}

} // namespace

int main()
{
    // prepare_mysql_test_env rather than a bare REVLM_TEST_MYSQL_DSN check: the
    // bare check made this test skip silently wherever that variable is unset,
    // so it could stay green for months without ever running. This starts its
    // own container when the variable is missing.
    const auto mysql_env = revlm::test::prepare_mysql_test_env("channel group contract");
    if (!mysql_env.has_value()) {
        return 0;
    }
    const std::string dsn = mysql_env->dsn;

    try {
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);

        reset_contract_tables(*db);

        revlm::Config config;
        config.db_dsn = dsn;
        revlm::test::install_test_runtime(config);

        revlm::ChannelGroupStore &group_store = revlm::ChannelGroupStore::instance();
        revlm::ChannelStore &channel_store = revlm::ChannelStore::instance();

        const long long group_id = group_store.create_channel_group("primary", "primary group", 1.0);

        revlm::Channel seed = make_channel("seed-channel", 11);
        revlm::Channel moved = make_channel("moved-channel", 7);
        revlm::Channel added = make_channel("added-channel", 42);
        if (!channel_store.create_channel(seed) || !channel_store.create_channel(moved) ||
            !channel_store.create_channel(added)) {
            std::cerr << "create channels failed\n";
            return 1;
        }

        // One way to put a channel in a group. The bulk-replace variant this used
        // to call had no other caller and meant two orderings to reason about.
        if (!group_store.add_channel_group_member(group_id, seed) ||
            !group_store.add_channel_group_member(group_id, moved) ||
            !group_store.add_channel_group_member(group_id, added)) {
            std::cerr << "add channel group members failed\n";
            return 1;
        }

        const revlm::ChannelGroup group = group_store.get_channel_group_by_id(group_id);
        if (expect(group.id == group_id, "group should load") != 0 ||
            expect(group.channels.size() == 3U, "group should have three channels in order") != 0 ||
            expect(group.channels[0].id == seed.id, "first member order should match seed") != 0 ||
            expect(group.channels[1].id == moved.id, "second member order should match moved") != 0 ||
            expect(group.channels[2].id == added.id, "third member should be added channel") != 0) {
            return 1;
        }

        revlm::Channel incompatible(0, "anthropic", "anthropic-channel", true, 1, "https://example.test", "key");
        if (!channel_store.create_channel(incompatible)) {
            std::cerr << "create incompatible channel failed\n";
            return 1;
        }
        bool rejected = false;
        try {
            (void)group_store.add_channel_group_member(group_id, incompatible);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        if (expect(rejected, "group should reject a channel from another plugin type") != 0 ||
            expect(group_store.get_channel_group_by_id(group_id).channels.size() == 3U,
                   "rejected member must not change the group") != 0) {
            return 1;
        }

        seed.type = "anthropic";
        rejected = false;
        try {
            (void)channel_store.update_channel(seed);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        if (expect(rejected, "channel type update should not make a mixed plugin group") != 0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "channel group contract test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
