#include "auth/users.hpp"
#include "billing/billing.hpp"
#include "errors/errors.hpp"
#include "models/models.hpp"
#include "models/quota.hpp"
#include "request/request.hpp"
#include "store/migrations.hpp"
#include "store/mysql.hpp"
#include "store/mysql_test_env.hpp"
#include "usage/usage_charge.hpp"
#include "usage/usage_commit_jobs.hpp"

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

long long create_test_user(revlm::MysqlConnection &conn)
{
    const std::string suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    conn.exec("INSERT INTO users(email,username,password_hash,role,status) VALUES(" +
              conn.quote("quota-" + suffix + "@example.com") + ", " + conn.quote("quota" + suffix) + ", " +
              conn.quote("$2b$12$placeholder") + ", 'user', 1)");
    return static_cast<long long>(conn.last_insert_id());
}

} // namespace

int main()
{
    try {
        const auto env = revlm::test::prepare_mysql_test_env("quota");
        if (!env.has_value()) {
            return 0;
        }
        (void)revlm::apply_migrations(env->dsn, "internal/store/migrations", "", 30);

        revlm::MysqlConnection conn(env->dsn);
        const long long broke_user_id = create_test_user(conn);
        const long long funded_user_id = create_test_user(conn);
        const long long token_id = 42;

        revlm::UserStore &users = revlm::UserStore::instance();
        users.reload(conn);
        revlm::User funded = users.get_user_by_id(funded_user_id);
        funded.balance_usd = "10.000000";
        (void)users.update_user(funded);
        revlm::BillingStore billing(conn);

        const std::vector<revlm::Model> &models = revlm::ModelManager::instance().models();
        const auto model_it = std::ranges::find(models, std::string{ "gpt-5.5" }, &revlm::Model::name);
        if (model_it == models.end()) {
            std::cerr << "builtin gpt-5.5 model missing\n";
            return 1;
        }
        const revlm::Model &model = *model_it;

        revlm::Request broke_request(model, 100'000, 50'000, 0, 0, 0);
        broke_request.user_id = broke_user_id;
        revlm::Quota quota(conn);
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
        funded_request.user_id = funded_user_id;
        revlm::UsageCommitPayload payload;
        payload.request_id = "req-quota-debit";
        payload.user_id = funded_user_id;
        payload.token_id = token_id;
        payload.model = "gpt-5.5";
        payload.direct_commit = true;
        payload.finalize.endpoint = "/v1/responses";
        payload.finalize.method = "POST";
        payload.finalize.status_code = 200;
        payload.finalize.is_stream = false;

        revlm::charge_request(conn, funded_request, payload);
        if (expect(payload.balance_debited, "charge_request should mark balance_debited") != 0 ||
            expect(payload.committed_usd != "0.000000", "successful charge should compute non-zero committed_usd") !=
                0) {
            return 1;
        }

        revlm::UsageCommitJobStore usage_store(conn);
        if (expect(usage_store.commit_usage_payload_direct(
                       revlm::UsageCommitJobInput{ payload.request_id, funded_user_id, token_id, payload },
                       revlm::usage_commit_timestamp_now()),
                   "direct usage commit should succeed") != 0) {
            return 1;
        }

        const std::string balance_after = billing.get_user_balance_usd(funded_user_id);
        if (expect(balance_after != "10.000000", "successful data-plane commit should debit user balance") != 0 ||
            expect(revlm::decimal_greater_than_zero(balance_after),
                   "debited user should still have readable balance") != 0) {
            return 1;
        }

        return 0;
    } catch (const std::exception &err) {
        std::cerr << "quota mysql test failed: " << err.what() << '\n';
        return 1;
    }
}
