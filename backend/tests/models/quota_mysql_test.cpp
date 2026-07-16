#include "users/users.hpp"
#include "errors/errors.hpp"
#include "models/models.hpp"
#include "models/quota.hpp"
#include "request/request.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"
#include "store/mysql_test_env.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
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
            __runtime_cfg.session_secret = "tmp-test-secret";
            revlm::test::install_test_runtime(__runtime_cfg);
        }

        const long long broke_user_id = create_test_user(*db);
        const long long funded_user_id = create_test_user(*db);
        const long long token_id = 42;

        revlm::UserStore &users = revlm::UserStore::instance();
        revlm::User funded = users.get_user_by_id(funded_user_id);
        funded.balance_usd = 10.0;
        (void)users.update_user(funded);

        const std::vector<revlm::Model> &models = revlm::ModelManager::instance().models();
        const auto model_it = std::ranges::find(models, std::string{ "gpt-5.5" }, &revlm::Model::name);
        if (model_it == models.end()) {
            std::cerr << "builtin gpt-5.5 model missing\n";
            return 1;
        }
        const revlm::Model &model = *model_it;

        revlm::Request broke_request(model, 100'000, 50'000, 0, 0, 0);
        broke_request.user_id = broke_user_id;
        revlm::Quota quota;
        bool insufficient = false;
        try {
            quota.charge(broke_request);
        } catch (const revlm::QuotaInsufficientBalanceError &) {
            insufficient = true;
        }
        if (expect(insufficient, "zero balance should reject charge") != 0) {
            return 1;
        }

        revlm::Request funded_request(model, 100'000, 50'000, 0, 0, 0);
        funded_request.id = 700001;
        funded_request.user_id = funded_user_id;
        funded_request.token_id = token_id;
        funded_request.endpoint = "/v1/responses";
        funded_request.method = "POST";
        funded_request.status_code = 200;
        funded_request.is_stream = false;

        revlm::Quota().charge(funded_request);
        if (expect(funded_request.solve_price() > 0.0, "successful charge should compute non-zero price") != 0) {
            return 1;
        }

        if (expect(funded_request.commit(revlm::request_timestamp_now()), "direct usage commit should succeed") != 0) {
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
