#include "config/config.hpp"
#include "users/users.hpp"
#include "users/tokens.hpp"
#include "store/database.hpp"
#include "store/mysql_test_env.hpp"
#include "store/schema.hpp"
#include "request/request.hpp"
#include "util/user_input.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <odb/database.hxx>
#include <odb/nullable.hxx>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

void exec_many(odb::database &db, const std::vector<std::string> &sqls)
{
    for (const std::string &sql : sqls) {
        revlm::sql_exec(db, sql);
    }
}

std::string insert_request(odb::database &db, long long id, std::string_view time, long long user_id,
                           long long token_id, std::string_view model, long long input_tokens, long long output_tokens)
{
    // ADR-0004: requests has no fixed token columns; token statistics live in
    // token_details JSON, persisted by core (usage_tokens extraction).
    revlm::json details;
    details["usage"]["input_tokens"] = input_tokens;
    details["usage"]["output_tokens"] = output_tokens;
    details["usage"]["cache_read_input_tokens"] = 0;
    details["usage"]["cache_creation"]["ephemeral_5m_input_tokens"] = 0;
    details["usage"]["cache_creation"]["ephemeral_1h_input_tokens"] = 0;
    return "INSERT INTO requests("
           "id,time,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
           "user_id,token_id,channel_id,model,"
           "token_details,channel_group_multiplier,is_stream) VALUES(" +
           std::to_string(id) + ",'" + std::string{ time } + "','/v1/responses','POST',200,100,20," +
           std::to_string(user_id) + "," + std::to_string(token_id) + ",0,'" + std::string{ model } + "'," +
           revlm::sql_quote(db, details.dump()) + ",1.0,0)";
}

} // namespace

int main()
{
    const char *dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping request totals MySQL test\n";
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

        exec_many(*db, {
                           "DELETE FROM request_totals",
                           "DELETE FROM requests",
                           "DELETE FROM user_tokens",
                           "DELETE FROM users",
                       });

        revlm::UserStore &users = revlm::UserStore::instance();
        revlm::TokenStore &tokens = users.tokens();

        revlm::User user("totals@example.com", "totalsuser", revlm::hash_password("password123"), "user");
        user.status = 1;
        const long long user_id = users.create_user(std::move(user));
        const long long token_id =
            tokens.create_user_token(user_id, odb::nullable<std::string>{ "totals token" }, "sk_totals_test_token");

        revlm::Request req;
        req.id = 1;
        req.user_id = user_id;
        req.token_id = token_id;
        req.time = "2026-06-20 12:00:00";
        req.date = "2026-06-20";
        req.model_name = "gpt-5.5";
        req.token_details = R"({"usage":{"input_tokens":100,"output_tokens":40,"cache_read_input_tokens":0,)"
                            R"("cache_creation":{"ephemeral_5m_input_tokens":0,"ephemeral_1h_input_tokens":0}}})";
        req.endpoint = "/v1/responses";
        req.method = "POST";
        req.status_code = 200;
        req.latency_ms = 120;
        req.first_token_latency_ms = 30;
        if (expect(req.commit("2026-06-20 12:00:05"), "commit should write requests row") != 0) {
            return 1;
        }
        const std::vector<revlm::RequestTotal> totals =
            tokens.requests().totals(user_id, token_id, "2026-06-20", "2026-06-20");
        if (expect(totals.size() == 1, "totals should contain one day") != 0 ||
            expect(totals[0].requests == 1, "totals requests mismatch") != 0) {
            return 1;
        }

        exec_many(*db, { insert_request(*db, 2, "2026-06-21 08:00:00", user_id, token_id, "gpt-5.5", 50, 20) });
        revlm::Request req2;
        req2.id = 2;
        req2.user_id = user_id;
        req2.token_id = token_id;
        req2.time = "2026-06-21 08:00:00";
        req2.date = "2026-06-21";
        req2.model_name = "gpt-5.5";
        req2.token_details = R"({"usage":{"input_tokens":50,"output_tokens":20,"cache_read_input_tokens":0,)"
                             R"("cache_creation":{"ephemeral_5m_input_tokens":0,"ephemeral_1h_input_tokens":0}}})";
        tokens.requests().apply_total(req2);

        const std::vector<revlm::RequestTotal> range_totals =
            tokens.requests().totals(user_id, token_id, "2026-06-20", "2026-06-21");
        long long sum_requests = 0;
        for (const revlm::RequestTotal &row : range_totals) {
            sum_requests += row.requests;
        }
        if (expect(sum_requests == 2, "range totals requests mismatch") != 0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "request totals MySQL test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
