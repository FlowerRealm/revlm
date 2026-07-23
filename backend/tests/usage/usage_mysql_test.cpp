#include "config/config.hpp"
#include "users/users.hpp"
#include "users/tokens.hpp"
#include "store/database.hpp"
#include "store/mysql_test_env.hpp"
#include "store/schema.hpp"
#include "request/request.hpp"
#include "util/user_input.hpp"

#include <cstdlib>
#include <ctime>
#include <exception>
#include <iostream>
#include <odb/database.hxx>
#include <odb/nullable.hxx>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

std::string env_or_empty(const char *key)
{
    const char *value = std::getenv(key);
    return value == nullptr ? std::string{} : std::string{ value };
}

std::string unique_name(std::string_view prefix)
{
    return std::string{ prefix } + std::to_string(static_cast<long long>(std::time(nullptr)));
}

long long create_user(odb::database &db, revlm::UserStore &users, std::string_view email, std::string_view username)
{
    (void)db;
    const revlm::User existing = users.get_user_by_email(email);
    if (existing.id != 0) {
        return existing.id;
    }
    revlm::User user(std::string{ email }, std::string{ username }, revlm::hash_password("password123"), "user");
    user.status = 1;
    return users.create_user(std::move(user));
}

} // namespace

int main()
{
    const std::string dsn = env_or_empty("REVLM_TEST_MYSQL_DSN");
    if (dsn.empty()) {
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping usage mysql test\n";
        return 0;
    }

    try {
        auto db = revlm::make_database(dsn);
        revlm::ensure_schema(*db);
        {
            revlm::Config __runtime_cfg;
            __runtime_cfg.db_dsn = dsn;
            revlm::test::install_test_runtime(__runtime_cfg);
        }

        revlm::UserStore &users = revlm::UserStore::instance();
        revlm::TokenStore &tokens = users.tokens();

        const std::string email = unique_name("tmp_usage") + "@example.com";
        const std::string username = unique_name("tmpusage");
        const long long user_id = create_user(*db, users, email, username);
        const std::string raw_token = revlm::new_random_token("sk_tmp_usage_", 24);
        const long long token_id =
            tokens.create_user_token(user_id, odb::nullable<std::string>{ "tmp usage token" }, raw_token);

        const long long event_id = 900001 + (static_cast<long long>(std::time(nullptr)) % 100000);
        revlm::Request request;
        request.id = event_id;
        request.user_id = user_id;
        request.token_id = token_id;
        request.time = "2026-06-23 12:00:00";
        request.model_name = "gpt-5.5";
        request.service_tier = "priority";
        request.input_tokens = 100;
        request.cache_read_tokens = 20;
        request.cache_creation_5m_tokens = 5;
        request.cache_creation_1h_tokens = 2;
        request.output_tokens = 60;
        request.tier_multiplier = 1.0;
        request.channel_multiplier = 1.0;
        request.endpoint = "/v1/responses";
        request.method = "POST";
        request.status_code = 200;
        request.latency_ms = 120;
        request.first_token_latency_ms = 30;
        request.channel_id = 11;
        request.is_stream = false;

        if (expect(request.commit("2026-06-23 12:00:05"), "direct commit should write requests row") != 0) {
            return 1;
        }

        revlm::RequestStore &requests = revlm::UserStore::instance().tokens().requests();
        const auto loaded_opt = requests.get_by_id(event_id);
        if (expect(loaded_opt.has_value(), "requests row should exist") != 0) {
            return 1;
        }
        const revlm::Request &loaded = *loaded_opt;
        if (expect(loaded.id == event_id, "loaded id should match") != 0 ||
            expect(loaded.user_id == user_id, "loaded user_id should match") != 0 ||
            expect(loaded.token_id == token_id, "loaded token_id should match") != 0 ||
            expect(!loaded.model_name.null() && *loaded.model_name == "gpt-5.5", "loaded model should match") != 0 ||
            expect(!loaded.service_tier.null() && *loaded.service_tier == "priority",
                   "service tier should persist as priority") != 0 ||
            expect(loaded.input_tokens == 100, "loaded input_tokens should match") != 0 ||
            expect(loaded.cache_read_tokens == 20, "loaded cache_read_tokens should match") != 0 ||
            expect(loaded.cache_creation_5m_tokens == 5, "loaded cache_creation_5m_tokens should match") != 0 ||
            expect(loaded.cache_creation_1h_tokens == 2, "loaded cache_creation_1h_tokens should match") != 0 ||
            expect(loaded.output_tokens == 60, "loaded output_tokens should match") != 0 ||
            expect(loaded.channel_id == 11, "loaded channel_id should match") != 0 ||
            expect(!loaded.endpoint.null() && *loaded.endpoint == "/v1/responses", "loaded endpoint should match") !=
                0 ||
            expect(loaded.status_code == 200, "loaded status_code should match") != 0 ||
            expect(loaded.latency_ms == 120, "loaded latency_ms should match") != 0 ||
            expect(loaded.first_token_latency_ms == 30, "loaded first_token_latency_ms should match") != 0 ||
            expect(!loaded.is_stream, "loaded is_stream should match") != 0) {
            return 1;
        }

        return 0;
    } catch (const std::exception &err) {
        std::cerr << "usage mysql test failed: " << err.what() << '\n';
        return 1;
    }
}
