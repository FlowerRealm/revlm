#include "config/config.hpp"
#include "models/models.hpp"
#include "proxy/gateway.hpp"
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
    revlm::sql_exec(db, "INSERT INTO users(email,username,password_hash,role,status) VALUES(" +
                            revlm::sql_quote(db, "quota-" + suffix + "@example.com") + ", " +
                            revlm::sql_quote(db, "quota" + suffix) + ", " + revlm::sql_quote(db, "$2b$12$placeholder") +
                            ", 'user', 1)");
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

        const revlm::Model &model = revlm::GPT_5_5;

        revlm::ProxyRequest broke_request;
        fill_pricing_from_model(broke_request.upstream.pricing, model);
        broke_request.upstream.model_name = model.name;
        broke_request.usage.input_tokens = 100'000;
        broke_request.usage.output_tokens = 50'000;
        broke_request.id = 700000;
        broke_request.auth.user_id = broke_user_id;
        broke_request.auth.token_id = 1;
        broke_request.upstream.channel_id = 1;
        if (expect(!revlm::commit_proxy_usage(broke_request), "zero balance should reject charge") != 0) {
            return 1;
        }

        revlm::ProxyRequest funded_request;
        fill_pricing_from_model(funded_request.upstream.pricing, model);
        funded_request.upstream.model_name = model.name;
        funded_request.usage.input_tokens = 100'000;
        funded_request.usage.output_tokens = 50'000;
        funded_request.id = 700001;
        funded_request.auth.user_id = funded_user_id;
        funded_request.auth.token_id = token_id;
        funded_request.http.path = "/v1/responses";
        funded_request.http.method = "POST";
        funded_request.upstream.status_code = 200;
        funded_request.upstream.channel_id = 1;
        funded_request.is_stream = false;

        if (expect(revlm::commit_proxy_usage(funded_request), "funded commit_proxy_usage should succeed") != 0) {
            return 1;
        }
        if (expect(compute_usd(funded_request) > 0.0, "successful charge should compute non-zero price") != 0) {
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
