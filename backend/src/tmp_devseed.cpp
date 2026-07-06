#include <exception>
#include <iostream>
#include <string>

#include "config/config.hpp"
#include "store/migrations.hpp"
#include "store/mysql.hpp"
#include "auth/users.hpp"
#include "util/user_input.hpp"
#include "util/strings.hpp"

namespace
{

constexpr const char *kRootEmail = "root@local";
constexpr const char *kRootUsername = "root";
constexpr const char *kRootPassword = "dev-only-root";
constexpr const char *kUserEmail = "user@local";
constexpr const char *kUserUsername = "user";
constexpr const char *kUserPassword = "dev-only-user";

long long ensure_user(revlm::UserStore &store, std::string_view email, std::string_view username,
                      std::string_view password, std::string_view role)
{
    const std::string normalized_email = revlm::lowercase_ascii(revlm::trim_ascii(email));
    if (normalized_email.empty()) {
        throw std::invalid_argument("邮箱不能为空");
    }
    if (auto existing = store.get_user_by_email(normalized_email); existing.has_value()) {
        return existing->id;
    }
    if (auto existing = store.get_user_by_username(username); existing.has_value()) {
        return existing->id;
    }

    revlm::CreateUserInput input;
    input.email = normalized_email;
    input.username = revlm::normalize_username(username);
    input.password_hash = revlm::hash_password(password);
    input.role = std::string{ role };
    return store.create_user(input);
}

} // namespace

int main()
{
    try {
        revlm::Config config = revlm::load_config_from_env();
        if (!revlm::role_requires_db(config.role)) {
            throw std::invalid_argument("tmp-devseed requires an api/all role with REVLM_DB_DSN");
        }

        const revlm::MigrationResult migrations = revlm::apply_migrations(config);
        revlm::MysqlConnection conn(config.db_dsn, revlm::mysql_client_multi_statements);
        revlm::UserStore store(conn);

        const long long root_id = ensure_user(store, kRootEmail, kRootUsername, kRootPassword, "root");
        const long long user_id = ensure_user(store, kUserEmail, kUserUsername, kUserPassword, "user");

        std::cout << "tmp-devseed ok\n";
        std::cout << "migrations_applied=" << migrations.applied << " migrations_total=" << migrations.total << '\n';
        std::cout << "root_id=" << root_id << " email=" << kRootEmail << " username=" << kRootUsername << '\n';
        std::cout << "user_id=" << user_id << " email=" << kUserEmail << " username=" << kUserUsername << '\n';
        return 0;
    } catch (const std::exception &err) {
        std::cerr << "tmp-devseed failed: " << err.what() << '\n';
        return 1;
    }
}
