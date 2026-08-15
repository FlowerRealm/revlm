#include "config/config.hpp"
#include "proxy/protocol_dispatch.hpp"
#include "request/proxy_request.hpp"
#include "store/database.hpp"
#include "store/mysql_test_env.hpp"
#include "store/schema.hpp"
#include "users/users.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <odb/database.hxx>
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

long long create_test_user(odb::database &db)
{
    const std::string suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    // balance_usd is NOT NULL with no schema default (ODB emits none), so a
    // partial INSERT is rejected outright under MySQL's strict mode.
    revlm::sql_exec(db, "INSERT INTO users(email,username,password_hash,role,status,balance_usd) VALUES(" +
                            revlm::sql_quote(db, "quota-" + suffix + "@example.com") + ", " +
                            revlm::sql_quote(db, "quota" + suffix) + ", " + revlm::sql_quote(db, "$2b$12$placeholder") +
                            ", 'user', 1, 0)");
    const auto id = revlm::sql_query_one(db, "SELECT LAST_INSERT_ID()");
    return id.has_value() ? std::stoll(*id) : 0;
}

} // namespace

int main()
{
    try {
        const auto env = revlm::test::prepare_mysql_test_env("quota");
        if (!env.has_value()) {
            return 0;
        }
        auto db = revlm::make_database(env->dsn);
        revlm::ensure_schema(*db);
        {
            revlm::Config __runtime_cfg;
            __runtime_cfg.db_dsn = env->dsn;
            revlm::test::install_test_runtime(__runtime_cfg);
        }

        const long long broke_user_id = create_test_user(*db);
        const long long funded_user_id = create_test_user(*db);
        const long long token_id = 42;

        revlm::UserStore &users = revlm::UserStore::instance();
        revlm::User funded = users.get_user_by_id(funded_user_id);
        funded.balance_usd = 10.0;
        (void)users.update_user(funded);

        // Pricing is plugin-side now (ADR 0004): the core just applies the
        // channel group multiplier to whatever protocol_cost_usd the plugin
        // handed it. Quota enforcement lives entirely in commit_proxy_request's
        // balance debit, so the test only needs a positive charge, not a model.
        revlm::ProxyRequest broke_request;
        broke_request.model_name = "gpt-5.5";
        broke_request.protocol_cost_usd = 5.0;
        broke_request.id = 700000;
        broke_request.user_id = broke_user_id;
        broke_request.token_id = 1;
        broke_request.channel_id = 1;
        if (expect(!revlm::commit_proxy_request(broke_request), "zero balance should reject charge") != 0) {
            return 1;
        }

        revlm::ProxyRequest funded_request;
        funded_request.model_name = "gpt-5.5";
        funded_request.protocol_cost_usd = 5.0;
        funded_request.id = 700001;
        funded_request.user_id = funded_user_id;
        funded_request.token_id = token_id;
        funded_request.path = "/v1/responses";
        funded_request.method = "POST";
        funded_request.status_code = 200;
        funded_request.channel_id = 1;

        if (expect(revlm::commit_proxy_request(funded_request), "funded commit_proxy_request should succeed") != 0) {
            return 1;
        }
        if (expect(funded_request.usd > 0.0, "successful charge should compute non-zero price") != 0) {
            return 1;
        }

        const double balance_after = users.get_user_balance_usd(funded_user_id);
        if (expect(balance_after != 10.0, "successful data-plane commit should debit user balance") != 0 ||
            expect(balance_after > 0, "debited user should still have readable balance") != 0) {
            return 1;
        }

        return 0;
    } catch (const std::exception &err) {
        std::cerr << "quota mysql test failed: " << err.what() << '\n';
        return 1;
    }
}
