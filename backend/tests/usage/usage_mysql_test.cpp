#include "store/migrations.hpp"
#include "store/mysql.hpp"
#include "server/tokens.hpp"
#include "usage/usage.hpp"
#include "auth/users.hpp"

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

std::string env_or_empty(const char *key)
{
    const char *value = std::getenv(key);
    return value == nullptr ? std::string{} : std::string{ value };
}

std::string unique_name(std::string_view prefix)
{
    return std::string{ prefix } + std::to_string(static_cast<long long>(std::time(nullptr)));
}

long long create_user(revlm::MysqlConnection &conn, revlm::UserStore &users, std::string_view email,
                      std::string_view username)
{
    (void)conn;
    const auto existing = users.get_user_by_email(email);
    if (existing.has_value()) {
        return existing->id;
    }
    return users.create_user(revlm::User(std::string{ email }, std::string{ username },
                                         revlm::hash_password("password123"), "user"));
}

std::optional<std::string> query_value(revlm::MysqlConnection &conn, std::string_view sql)
{
    return conn.query_one(sql);
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
        const revlm::MigrationResult migrated = revlm::apply_migrations(dsn, "internal/store/migrations", "", 30);
        if (migrated.total < 120) {
            std::cerr << "unexpected migration count\n";
            return 1;
        }

        revlm::MysqlConnection conn(dsn, revlm::mysql_client_multi_statements);
        revlm::UserStore users(conn);
        revlm::TokenStore tokens(conn);
        revlm::UsageStore usage(conn);

        const std::string run_tag = std::to_string(static_cast<long long>(std::time(nullptr)));
        const std::string email = unique_name("tmp_usage") + "@example.com";
        const std::string username = unique_name("tmpusage");
        const long long user_id = create_user(conn, users, email, username);
        const std::string raw_token = revlm::new_random_token("sk_tmp_usage_", 24);
        const long long token_id = tokens.create_user_token(user_id, std::string{ "tmp usage token" }, raw_token);

        const std::string request_one = "tmp-d014-usage-1-" + run_tag;
        const std::string request_failed = "tmp-d014-usage-failed-" + run_tag;
        const std::string request_expire = "tmp-d014-usage-expire-" + run_tag;
        const std::string request_staged = "tmp-d014-usage-staged-" + run_tag;
        const std::string request_staged_rollback = "tmp-d014-usage-staged-rollback-" + run_tag;

        revlm::UsageBeginInput begin_one{};
        begin_one.request_id = request_one;
        begin_one.user_id = user_id;
        begin_one.token_id = token_id;
        begin_one.model = std::string{ "gpt-5.2" };
        begin_one.requested_service_tier = std::string{ " fast " };
        begin_one.service_tier = std::string{ "default" };
        begin_one.service_tier_downgrade_reason = std::string{ "fast_unavailable" };
        begin_one.committed_usd = "1.230000";
        const long long usage_id = usage.begin_usage(begin_one);

        revlm::UsageBeginInput begin_duplicate{};
        begin_duplicate.request_id = request_one;
        begin_duplicate.user_id = user_id;
        begin_duplicate.token_id = token_id;
        begin_duplicate.model = std::string{ "gpt-5.2" };
        const auto duplicate_id = usage.begin_usage(begin_duplicate);
        if (expect(usage_id == duplicate_id, "duplicate request_id should reuse the same row") != 0) {
            return 1;
        }
        const auto begun = usage.get_usage_event_by_id(usage_id);
        if (expect(begun.has_value(), "begun usage row should exist") != 0 ||
            expect(begun->state == revlm::usage_state_pending, "begin usage should write pending state") != 0 ||
            expect(begun->committed_usd == "1.230000", "begin usage should keep pending hold in committed_usd") != 0 ||
            expect(begun->requested_service_tier.value_or("") == "priority", "requested tier should normalize") != 0) {
            return 1;
        }
        if (expect(query_value(conn, "SELECT COUNT(*) FROM usage_events WHERE request_id=" + conn.quote(request_one))
                           .value_or("0") == "1",
                   "duplicate request_id should leave one usage_events row") != 0) {
            return 1;
        }

        revlm::UsageCommitInput commit_one{};
        commit_one.usage_event_id = usage_id;
        commit_one.channel_id = 11;
        commit_one.input_tokens = 100;
        commit_one.cache_read_input_tokens = 20;
        commit_one.cache_creation_input_tokens = 5;
        commit_one.cache_creation_1h_input_tokens = 2;
        commit_one.output_tokens = 60;
        commit_one.committed_usd = "2.500000";
        commit_one.price_multiplier = "1.500000";
        commit_one.price_multiplier_group = "1.200000";
        commit_one.price_multiplier_payment = "1.100000";
        commit_one.price_multiplier_group_name = std::string{ "default/main" };
        if (expect(usage.commit_usage(commit_one), "commit usage should update pending row") != 0) {
            return 1;
        }
        const auto committed = usage.get_usage_event_by_id(usage_id);
        if (expect(committed.has_value(), "committed row should still exist") != 0 ||
            expect(committed->state == revlm::usage_state_committed, "commit should move state to committed") != 0 ||
            expect(committed->channel_id.value_or(0) == 11, "commit should store upstream channel") != 0 ||
            expect(committed->cache_creation_1h_input_tokens.value_or(0) == 2, "commit should store cache 1h tokens") !=
                0 ||
            expect(committed->price_multiplier_group_name.value_or("") == "default/main",
                   "commit should store price group path") != 0) {
            return 1;
        }

        revlm::UsageBeginInput stale_duplicate{};
        stale_duplicate.request_id = request_one;
        stale_duplicate.user_id = user_id;
        stale_duplicate.token_id = token_id;
        bool stale_duplicate_failed = false;
        try {
            (void)usage.begin_usage(stale_duplicate);
        } catch (const std::runtime_error &) {
            stale_duplicate_failed = true;
        }
        if (expect(stale_duplicate_failed, "duplicate request_id on non-pending row should fail") != 0) {
            return 1;
        }

        revlm::UsageFinalizeInput finalize_one{};
        finalize_one.usage_event_id = usage_id;
        finalize_one.endpoint = std::string{ "/v1/responses" };
        finalize_one.method = std::string{ "POST" };
        finalize_one.status_code = 200;
        finalize_one.latency_ms = 120;
        finalize_one.first_token_latency_ms = 30;
        finalize_one.forwarded_model = std::string{ "gpt-5.2-mini" };
        finalize_one.upstream_response_model = std::string{ "gpt-5.2-upstream" };
        finalize_one.channel_id = 11;
        finalize_one.is_stream = false;
        finalize_one.request_bytes = 777;
        finalize_one.response_bytes = 999;
        if (expect(usage.finalize_usage(finalize_one), "finalize usage should write metadata") != 0) {
            return 1;
        }
        const auto finalized = usage.get_usage_event_by_id(usage_id);
        if (expect(finalized.has_value(), "finalized row should exist") != 0 ||
            expect(finalized->endpoint.value_or("") == "/v1/responses", "finalize should store endpoint") != 0 ||
            expect(finalized->forwarded_model.value_or("") == "gpt-5.2-mini",
                   "finalize should store forwarded model") != 0 ||
            expect(finalized->response_bytes == 999, "finalize should store response bytes") != 0) {
            return 1;
        }

        revlm::UsageFinalizeInput finalize_sparse{};
        finalize_sparse.usage_event_id = usage_id;
        finalize_sparse.status_code = 202;
        finalize_sparse.latency_ms = 140;
        if (expect(usage.finalize_usage(finalize_sparse), "sparse finalize should still succeed") != 0) {
            return 1;
        }
        const auto finalized_sparse = usage.get_usage_event_by_id(usage_id);
        if (expect(finalized_sparse.has_value(), "sparse finalized row should exist") != 0 ||
            expect(finalized_sparse->channel_id.value_or(0) == 11, "finalize should not clear upstream channel") != 0 ||
            expect(finalized_sparse->forwarded_model.value_or("") == "gpt-5.2-mini",
                   "finalize should not clear forwarded model") != 0 ||
            expect(finalized_sparse->endpoint.value_or("") == "/v1/responses", "finalize should not clear endpoint") !=
                0) {
            return 1;
        }

        revlm::UsageBeginInput failed_begin{};
        failed_begin.request_id = request_failed;
        failed_begin.user_id = user_id;
        failed_begin.token_id = token_id;
        const long long failed_id = usage.begin_usage(failed_begin);
        if (expect(usage.abort_usage(failed_id), "abort should transition pending usage") != 0) {
            return 1;
        }
        const auto failed = usage.get_usage_event_by_id(failed_id);
        if (expect(failed.has_value(), "aborted row should exist") != 0 ||
            expect(failed->state == revlm::usage_state_failed, "abort should mark failed state") != 0 ||
            expect(failed->error_class.value_or("") == "aborted", "abort should store aborted class") != 0) {
            return 1;
        }

        revlm::UsageBeginInput expiring_begin{};
        expiring_begin.request_id = request_expire;
        expiring_begin.user_id = user_id;
        expiring_begin.token_id = token_id;
        const long long expiring_id = usage.begin_usage(expiring_begin);
        conn.exec("UPDATE usage_events SET created_at='2000-01-01 00:00:00' WHERE id=" + std::to_string(expiring_id));
        const revlm::UsageExpireResult expired = usage.expire_pending_usage("2001-01-01 00:00:00");
        if (expect(expired.expired == 1, "expire should report real expired row count") != 0 ||
            expect(expired.ids.size() == 1 && expired.ids[0] == expiring_id,
                   "expire should return only successfully expired ids") != 0) {
            return 1;
        }
        const auto expired_row = usage.get_usage_event_by_id(expiring_id);
        if (expect(expired_row.has_value(), "expired row should exist") != 0 ||
            expect(expired_row->state == revlm::usage_state_failed, "expire should mark failed state") != 0 ||
            expect(expired_row->error_class.value_or("") == "expired", "expire should mark expired class") != 0) {
            return 1;
        }

        revlm::UsageBeginInput staged_begin{};
        staged_begin.request_id = request_staged;
        staged_begin.user_id = user_id;
        staged_begin.token_id = token_id;
        const long long staged_id = usage.begin_usage(staged_begin);
        std::vector<std::string> order;
        revlm::UsageCommitInput staged_commit{};
        staged_commit.usage_event_id = staged_id;
        staged_commit.committed_usd = "0.500000";
        revlm::UsageFinalizeInput staged_finalize{};
        staged_finalize.usage_event_id = staged_id;
        staged_finalize.endpoint = std::string{ "/v1/responses" };
        staged_finalize.method = std::string{ "POST" };
        staged_finalize.status_code = 201;
        staged_finalize.latency_ms = 80;
        const bool staged_ok = usage.commit_non_stream_before_response(staged_commit, staged_finalize, [&]() {
            const auto row = usage.get_usage_event_by_id(staged_id);
            if (!row.has_value() || row->state != revlm::usage_state_committed || row->status_code != 201) {
                throw std::runtime_error("response action observed uncommitted usage");
            }
            order.push_back("response");
        });
        if (expect(staged_ok, "non-stream staged helper should succeed") != 0 ||
            expect(order.size() == 1 && order[0] == "response",
                   "response should run only after commit/finalize succeed") != 0) {
            return 1;
        }

        revlm::UsageBeginInput rollback_begin{};
        rollback_begin.request_id = request_staged_rollback;
        rollback_begin.user_id = user_id;
        rollback_begin.token_id = token_id;
        const long long rollback_id = usage.begin_usage(rollback_begin);
        revlm::UsageCommitInput rollback_commit{};
        rollback_commit.usage_event_id = rollback_id;
        rollback_commit.committed_usd = "0.250000";
        revlm::UsageFinalizeInput rollback_finalize{};
        rollback_finalize.usage_event_id = 0;
        rollback_finalize.status_code = 204;
        bool rollback_threw = false;
        try {
            (void)usage.commit_non_stream_before_response(rollback_commit, rollback_finalize, []() {});
        } catch (const std::invalid_argument &) {
            rollback_threw = true;
        }
        const auto rollback_row = usage.get_usage_event_by_id(rollback_id);
        if (expect(rollback_threw, "staged helper should roll back when finalize throws") != 0 ||
            expect(rollback_row.has_value(), "rollback row should still exist") != 0 ||
            expect(rollback_row->state == revlm::usage_state_pending,
                   "failed staged helper should roll committed state back") != 0 ||
            expect(rollback_row->status_code == 0, "failed staged helper should not leave finalized metadata behind") !=
                0) {
            return 1;
        }

        return 0;
    } catch (const std::exception &err) {
        std::cerr << "usage mysql test failed: " << err.what() << '\n';
        return 1;
    }
}
