#include "auth/users.hpp"
#include "server/tokens.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"
#include "request/request.hpp"
#include "util/user_input.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
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

std::string insert_request(long long id, std::string_view time, long long user_id, long long token_id,
                           std::string_view model, long long input_tokens, long long output_tokens)
{
    return "INSERT INTO requests("
           "id,time,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
           "user_id,token_id,channel_id,status,model,"
           "input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
           "output_tokens,tier_multiplier,channel_multiplier,is_stream) VALUES(" +
           std::to_string(id) + ",'" + std::string{ time } +
           "','/v1/responses','POST',200,100,20," + std::to_string(user_id) + "," + std::to_string(token_id) +
           ",0,'committed','" + std::string{ model } + "'," + std::to_string(input_tokens) + ",0,0,0," +
           std::to_string(output_tokens) + ",1.0,1.0,0)";
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

        exec_many(*db, {
                            "DELETE FROM request_totals",
                            "DELETE FROM requests",
                            "DELETE FROM user_tokens",
                            "DELETE FROM users",
                        });

        revlm::UserStore users(*db);
        revlm::TokenStore &tokens = users.tokens();

        revlm::User user("totals@example.com", "totalsuser", revlm::hash_password("password123"), "user");
        user.status = 1;
        const long long user_id = users.create_user(std::move(user));
        const long long token_id =
            tokens.create_user_token(user_id, odb::nullable<std::string>{"totals token"}, "sk_totals_test_token");

        revlm::Request req;
        req.id = 1;
        req.user_id = user_id;
        req.token_id = token_id;
        req.time = "2026-06-20 12:00:00";
        req.date = "2026-06-20";
        req.model.name = "gpt-5.5";
        req.input_tokens = 100;
        req.output_tokens = 40;
        req.tier_multiplier = 1.0;
        req.channel_multiplier = 1.0;
        req.endpoint = "/v1/responses";
        req.method = "POST";
        req.status_code = 200;
        req.latency_ms = 120;
        req.first_token_latency_ms = 30;
        req.statue = true;
        if (expect(req.commit(*db, "2026-06-20 12:00:05"), "commit should write requests row") != 0) {
            return 1;
        }
        const std::vector<revlm::RequestTotal> totals =
            tokens.requests().totals(user_id, token_id, "2026-06-20", "2026-06-20");
        if (expect(totals.size() == 1, "totals should contain one day") != 0 ||
            expect(totals[0].requests == 1, "totals requests mismatch") != 0 ||
            expect(totals[0].input_tokens == 100, "totals input_tokens mismatch") != 0 ||
            expect(totals[0].output_tokens == 40, "totals output_tokens mismatch") != 0) {
            return 1;
        }

        exec_many(*db, { insert_request(2, "2026-06-21 08:00:00", user_id, token_id, "gpt-5.5", 50, 20) });
        revlm::Request req2;
        req2.id = 2;
        req2.user_id = user_id;
        req2.token_id = token_id;
        req2.time = "2026-06-21 08:00:00";
        req2.date = "2026-06-21";
        req2.model.name = "gpt-5.5";
        req2.input_tokens = 50;
        req2.output_tokens = 20;
        req2.statue = true;
        tokens.requests().apply_committed(req2);

        const std::vector<revlm::RequestTotal> range_totals =
            tokens.requests().totals(user_id, token_id, "2026-06-20", "2026-06-21");
        long long sum_requests = 0;
        long long sum_input = 0;
        for (const revlm::RequestTotal &row : range_totals) {
            sum_requests += row.requests;
            sum_input += row.input_tokens;
        }
        if (expect(sum_requests == 2, "range totals requests mismatch") != 0 ||
            expect(sum_input == 150, "range totals input mismatch") != 0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "request totals MySQL test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
