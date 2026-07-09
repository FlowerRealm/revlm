#include "auth/users.hpp"
#include "billing/billing.hpp"
#include "store/migrations.hpp"
#include "store/mysql.hpp"
#include "store/mysql_test_env.hpp"

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

std::string unique_email()
{
    return "billing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "@example.com";
}

long long create_test_user(revlm::MysqlConnection &conn)
{
    conn.exec("INSERT INTO users(email,username,password_hash,role,status) VALUES(" +
              conn.quote(unique_email()) + ", " +
              conn.quote("billingUser" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) +
              ", " + conn.quote("$2b$12$placeholder") + ", 'user', 1)");
    return static_cast<long long>(conn.last_insert_id());
}

} // namespace

int main()
{
    try {
        const auto env = revlm::test::prepare_mysql_test_env("billing");
        if (!env.has_value()) {
            return 0;
        }
        (void)revlm::apply_migrations(env->dsn, "internal/store/migrations", "", 30);

        revlm::MysqlConnection conn(env->dsn);
        const long long user_id = create_test_user(conn);
        const long long zero_user_id = create_test_user(conn);
        revlm::BillingStore store(conn);
        revlm::UserStore &users = revlm::UserStore::instance();
        users.reload(conn);

        if (expect(store.get_user_balance_usd(user_id) == "0.000000", "new user balance should default to zero") != 0 ||
            expect(!store.has_positive_user_balance(user_id), "new user should not have positive balance") != 0) {
            return 1;
        }
        std::string remaining;
        if (expect(!store.debit_user_balance_usd(zero_user_id, 0.000001, &remaining) && remaining == "0.000000" &&
                       store.get_user_balance_usd(zero_user_id) == "0.000000",
                   "persisted zero balance should stay readable and insufficient debits should return false") != 0) {
            return 1;
        }

        revlm::User funded = users.get_user_by_id(user_id);
        funded.balance_usd = "27.750000";
        if (expect(users.update_user(funded) && store.get_user_balance_usd(user_id) == "27.750000" &&
                       store.has_positive_user_balance(user_id),
                   "admin balance top-up should credit user balance") != 0) {
            return 1;
        }

        if (expect(store.debit_user_balance_usd(user_id, 13.75, &remaining) && remaining == "14.000000",
                   "debit_user_balance_usd should atomically subtract balance") != 0 ||
            expect(!store.debit_user_balance_usd(user_id, 129.0, &remaining) && remaining == "14.000000",
                   "debit_user_balance_usd should reject insufficient balance") != 0) {
            return 1;
        }
        if (expect(store.debit_user_balance_usd(user_id, 14.0, &remaining) && remaining == "0.000000" &&
                       store.get_user_balance_usd(user_id) == "0.000000" && !store.has_positive_user_balance(user_id),
                   "exact spend-down to zero should persist as a valid balance") != 0 ||
            expect(!store.debit_user_balance_usd(user_id, 0.000001, &remaining) && remaining == "0.000000",
                   "zero persisted balance should not throw on later insufficient debits") != 0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "billing MySQL smoke failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
