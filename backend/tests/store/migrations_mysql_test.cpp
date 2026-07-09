#include "store/migrations.hpp"
#include "store/mysql_test_env.hpp"

#include <iostream>

int main()
{
    try {
        const auto env = revlm::test::prepare_mysql_test_env("migrations");
        if (!env.has_value()) {
            return 0;
        }
        const revlm::MigrationResult first = revlm::apply_migrations(env->dsn, "internal/store/migrations", "", 30);
        if (first.total != 2 || first.applied != 2) {
            std::cerr << "fresh migration count is unexpected: applied=" << first.applied << " total=" << first.total
                      << '\n';
            return 1;
        }
        const revlm::MigrationResult second = revlm::apply_migrations(env->dsn, "internal/store/migrations", "", 30);
        if (second.total != first.total || second.applied != 0) {
            std::cerr << "second migration pass should be idempotent: applied=" << second.applied
                      << " total=" << second.total << '\n';
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "migration smoke failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
