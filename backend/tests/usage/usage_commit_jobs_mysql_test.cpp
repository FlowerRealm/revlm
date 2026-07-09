#include "auth/users.hpp"
#include "util/user_input.hpp"
#include "runtime/runtime_workers.hpp"
#include "server/tokens.hpp"
#include "store/migrations.hpp"
#include "usage/usage_commit_jobs.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
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

revlm::Request make_request(long long usage_event_id, long long user_id, long long token_id)
{
    revlm::Request request;
    request.id = usage_event_id;
    request.user_id = user_id;
    request.token_id = token_id;
    request.time = "2026-06-23 12:00:00";
    request.model.name = "gpt-5";
    request.service_tier = "default";
    request.input_tokens = 12;
    request.cache_read_tokens = 2;
    request.cache_creation_5m_tokens = 3;
    request.cache_creation_1h_tokens = 1;
    request.output_tokens = 34;
    request.tier_multiplier = 1.5;
    request.channel_multiplier = 1.1;
    request.endpoint = "/v1/responses";
    request.method = "POST";
    request.status_code = 200;
    request.latency_ms = 128;
    request.first_token_latency_ms = 45;
    request.channel_id = 3;
    request.is_stream = true;
    request.statue = true;
    return request;
}

revlm::UsageCommitJobInput make_job_input(long long usage_event_id, long long user_id, long long token_id,
                                          bool direct_commit = false, bool balance_debited = false,
                                          bool retryable = false)
{
    return revlm::UsageCommitJobInput{ usage_event_id, user_id, token_id,
                                       make_request(usage_event_id, user_id, token_id), direct_commit,
                                       balance_debited, retryable };
}

revlm::UsageCommitFinalizeInput make_finalize(long long job_id, const revlm::Request &request,
                                              std::string_view finished_at, bool balance_debited = false,
                                              bool retryable = false)
{
    return revlm::UsageCommitFinalizeInput{
        job_id, std::string{ revlm::usage_commit_job_state_streaming },
        std::string{ revlm::usage_commit_job_state_ready }, request, balance_debited, retryable,
        std::string{ finished_at }
    };
}

std::string timestamp_offset_seconds(long long seconds)
{
    const auto when = std::chrono::system_clock::now() + std::chrono::seconds(seconds);
    const std::time_t t = std::chrono::system_clock::to_time_t(when);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string{ buffer };
}

bool has_job_id(const std::vector<revlm::UsageCommitJob> &jobs, long long id)
{
    for (const auto &job : jobs) {
        if (job.id == id) {
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    const char *dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping usage commit MySQL smoke\n";
        return 0;
    }

    try {
        auto step = [](const char *name) { std::cerr << "[usage-commit-smoke] " << name << '\n'; };
        step("migrate");
        (void)revlm::apply_migrations(dsn, "internal/store/migrations", "", 30);
        revlm::MysqlConnection conn(dsn, revlm::mysql_client_multi_statements);
        conn.exec("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
        conn.exec("TRUNCATE TABLE usage_commit_jobs");
        conn.exec("TRUNCATE TABLE usage_events");
        conn.exec("DELETE FROM user_balances");
        for (long long user_id = 101; user_id <= 113; ++user_id) {
            conn.exec("INSERT INTO user_balances(user_id, usd, created_at, updated_at) VALUES(" +
                      std::to_string(user_id) + ", 100.000000, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)");
        }

        revlm::UsageCommitJobStore store(conn);

        step("create-finalize-claim-complete-single");
        constexpr long long event_a = 1001;
        const auto request_a = make_request(event_a, 101, 201);
        const long long job_a =
            store.create_usage_commit_job(make_job_input(event_a, 101, 201));
        if (expect(job_a > 0, "create_usage_commit_job should return id") != 0) {
            return 1;
        }

        if (expect(store.finalize_usage_commit_job(make_finalize(job_a, request_a, "2026-06-23 12:00:05")),
                   "finalize_usage_commit_job should move streaming to ready") != 0) {
            return 1;
        }

        const auto claimed = store.claim_ready_usage_commit_jobs(
            revlm::UsageCommitClaimInput{ 8, "lease-a", timestamp_offset_seconds(300) });
        if (expect(claimed.size() == 1, "claim should fetch ready job") != 0 ||
            expect(claimed[0].id == job_a, "claimed job should match created job") != 0 ||
            expect(claimed[0].state == revlm::usage_commit_job_state_processing,
                   "claimed job should move to processing") != 0) {
            return 1;
        }

        if (expect(store.extend_processing_usage_commit_jobs_lease("lease-a", timestamp_offset_seconds(600)),
                   "extend_processing_usage_commit_jobs_lease should touch lease") != 0 ||
            expect(store.complete_usage_commit_job(job_a, "lease-a", "2026-06-23 12:02:05"),
                   "complete_usage_commit_job should succeed") != 0 ||
            expect(store.count_usage_events_by_id(event_a) == 1, "complete should write one usage event") != 0 ||
            expect(store.usage_event_status_by_id(event_a).value_or("") == "committed",
                   "usage event should be committed") != 0) {
            return 1;
        }

        step("batch-fast-path");
        constexpr long long event_b = 1002;
        constexpr long long event_c = 1003;
        const auto request_b = make_request(event_b, 102, 202);
        const auto request_c = make_request(event_c, 103, 203);
        const auto fast_ids = store.create_usage_commit_jobs_fast({
            make_job_input(event_b, 102, 202),
            make_job_input(event_c, 103, 203),
        });
        if (expect(fast_ids.size() == 2, "create_usage_commit_jobs_fast should preserve batch size") != 0) {
            return 1;
        }
        if (expect(store.finalize_usage_commit_job(make_finalize(0, request_b, "2026-06-23 12:03:00")),
                   "finalize by usage_event_id should work for event_b") != 0 ||
            expect(store.finalize_usage_commit_job(make_finalize(0, request_c, "2026-06-23 12:03:01")),
                   "finalize by usage_event_id should work for event_c") != 0) {
            return 1;
        }

        const auto claimed_batch = store.claim_ready_usage_commit_jobs(
            revlm::UsageCommitClaimInput{ 8, "lease-batch", timestamp_offset_seconds(300) });
        if (expect(claimed_batch.size() == 2, "claim should batch multiple ready jobs") != 0) {
            return 1;
        }
        const auto completion = store.complete_usage_commit_jobs(claimed_batch, "lease-batch", "2026-06-23 12:03:40");
        if (expect(completion.completed == 2, "batch complete should fast-path both jobs") != 0 ||
            expect(completion.dead_lettered == 0, "batch complete should not dead-letter good jobs") != 0 ||
            expect(store.count_usage_events_by_id(event_b) == 1, "event_b should be committed once") != 0 ||
            expect(store.count_usage_events_by_id(event_c) == 1, "event_c should be committed once") != 0) {
            return 1;
        }

        step("expired-processing-reclaim");
        constexpr long long event_reclaim_stale = 1004;
        constexpr long long event_reclaim_ready = 1005;
        const auto request_reclaim_stale = make_request(event_reclaim_stale, 109, 209);
        const auto request_reclaim_ready = make_request(event_reclaim_ready, 110, 210);
        const long long reclaim_stale_id = store.create_usage_commit_job(
            revlm::UsageCommitJobInput{ event_reclaim_stale, 109, 209, request_reclaim_stale });
        const long long reclaim_ready_id = store.create_usage_commit_job(
            revlm::UsageCommitJobInput{ event_reclaim_ready, 110, 210, request_reclaim_ready });
        if (expect(store.finalize_usage_commit_job(make_finalize(reclaim_stale_id, request_reclaim_stale, "2026-06-23 12:04:00")),
                   "reclaim stale finalize should work") != 0 ||
            expect(store.finalize_usage_commit_job(make_finalize(reclaim_ready_id, request_reclaim_ready, "2026-06-23 12:04:01")),
                   "reclaim ready finalize should work") != 0) {
            return 1;
        }
        const auto reclaim_first_claim = store.claim_ready_usage_commit_jobs(
            revlm::UsageCommitClaimInput{ 1, "lease-reclaim-old", timestamp_offset_seconds(300) });
        if (expect(reclaim_first_claim.size() == 1 && reclaim_first_claim[0].id == reclaim_stale_id,
                   "first reclaim claim should grab stale candidate") != 0) {
            return 1;
        }
        conn.exec("UPDATE usage_commit_jobs SET lease_until='2000-01-01 00:00:00' WHERE id=" +
                  std::to_string(reclaim_stale_id));
        const auto reclaim_mixed_claim = store.claim_ready_usage_commit_jobs(
            revlm::UsageCommitClaimInput{ 2, "lease-reclaim-new", timestamp_offset_seconds(300) });
        if (expect(reclaim_mixed_claim.size() == 2, "claim should return expired processing and ready jobs together") !=
                0 ||
            expect(has_job_id(reclaim_mixed_claim, reclaim_stale_id),
                   "expired processing job should not starve behind ready backlog") != 0 ||
            expect(has_job_id(reclaim_mixed_claim, reclaim_ready_id),
                   "claim should still include ready work after reclaiming expired processing") != 0) {
            return 1;
        }
        (void)store.complete_usage_commit_jobs(reclaim_mixed_claim, "lease-reclaim-new", "2026-06-23 12:04:20");
        if (expect(store.count_usage_events_by_id(event_reclaim_stale) == 1,
                   "reclaimed stale job should still commit once") != 0 ||
            expect(store.count_usage_events_by_id(event_reclaim_ready) == 1,
                   "ready job should commit alongside reclaimed stale job") != 0) {
            return 1;
        }

        step("stale-abort");
        constexpr long long event_stale = 1006;
        const auto request_stale = make_request(event_stale, 104, 204);
        const long long stale_id =
            store.create_usage_commit_job(make_job_input(event_stale, 104, 204));
        if (expect(stale_id > 0, "stale job create should return id") != 0) {
            return 1;
        }
        conn.exec("UPDATE usage_commit_jobs SET updated_at='2026-06-23 11:00:00' WHERE id=" + std::to_string(stale_id));
        (void)store.abort_stale_streaming_usage_commit_jobs("2026-06-23 11:30:00");
        auto stale_job = store.get_usage_commit_job_by_id(stale_id);
        if (expect(stale_job.has_value() && stale_job->state == revlm::usage_commit_job_state_aborted,
                   "abort_stale_streaming_usage_commit_jobs should mark stale streaming rows aborted") != 0) {
            return 1;
        }

        step("retryable-requeue");
        constexpr long long event_retry = 1007;
        const auto request_retry = make_request(event_retry, 9999, 205);
        const long long retry_id =
            store.create_usage_commit_job(make_job_input(event_retry, 9999, 205, true, false, true));
        if (expect(store.finalize_usage_commit_job(make_finalize(retry_id, request_retry, "2026-06-23 12:05:00", false, true)),
                   "retry finalize should work") != 0) {
            return 1;
        }
        const auto retry_claimed = store.claim_ready_usage_commit_jobs(
            revlm::UsageCommitClaimInput{ 1, "lease-retry", timestamp_offset_seconds(300) });
        if (expect(retry_claimed.size() == 1, "retry claim should pick one job") != 0) {
            return 1;
        }
        (void)store.complete_usage_commit_job(retry_id, "lease-retry", "2026-06-23 12:05:40");
        auto retry_job = store.get_usage_commit_job_by_id(retry_id);
        if (expect(retry_job.has_value() && retry_job->state == revlm::usage_commit_job_state_ready,
                   "retryable failure should requeue to ready") != 0) {
            return 1;
        }

        conn.exec("UPDATE usage_commit_jobs SET state='dead_letter' WHERE id=" + std::to_string(retry_id));

        step("dead-letter");
        constexpr long long event_dead = 1008;
        const auto request_dead = make_request(event_dead, 9998, 206);
        const long long dead_id =
            store.create_usage_commit_job(make_job_input(event_dead, 9998, 206, true, false, false));
        if (expect(store.finalize_usage_commit_job(make_finalize(dead_id, request_dead, "2026-06-23 12:06:00")),
                   "dead-letter finalize should work") != 0) {
            return 1;
        }
        const auto dead_claimed = store.claim_ready_usage_commit_jobs(
            revlm::UsageCommitClaimInput{ 1, "lease-dead", timestamp_offset_seconds(300) });
        if (expect(dead_claimed.size() == 1, "dead claim should pick one job") != 0) {
            return 1;
        }
        (void)store.complete_usage_commit_job(dead_id, "lease-dead", "2026-06-23 12:06:40");
        auto dead_job = store.get_usage_commit_job_by_id(dead_id);
        if (expect(dead_job.has_value() && dead_job->state == revlm::usage_commit_job_state_dead_letter,
                   "non-retryable failure should dead-letter") != 0) {
            return 1;
        }

        step("stale-lease-reclaim");
        constexpr long long event_lease = 1009;
        auto request_lease = make_request(event_lease, 111, 211);
        const long long lease_job_id =
            store.create_usage_commit_job(make_job_input(event_lease, 111, 211));
        if (expect(store.finalize_usage_commit_job(make_finalize(lease_job_id, request_lease, "2026-06-23 12:06:55")),
                   "lease job finalize should work") != 0) {
            return 1;
        }
        const auto lease_first_claim = store.claim_ready_usage_commit_jobs(
            revlm::UsageCommitClaimInput{ 1, "lease-old", timestamp_offset_seconds(300) });
        if (expect(lease_first_claim.size() == 1 && lease_first_claim[0].id == lease_job_id,
                   "lease job should be claimable") != 0) {
            return 1;
        }
        conn.exec("UPDATE usage_commit_jobs SET lease_until='2000-01-01 00:00:00' WHERE id=" +
                  std::to_string(lease_job_id));
        if (expect(!store.complete_usage_commit_job(lease_job_id, "lease-old", "2026-06-23 12:07:00"),
                   "stale lease holder must not commit") != 0 ||
            expect(store.count_usage_events_by_id(event_lease) == 0,
                   "usage event should stay absent after stale lease rejection") != 0) {
            return 1;
        }
        const auto lease_second_claim = store.claim_ready_usage_commit_jobs(
            revlm::UsageCommitClaimInput{ 1, "lease-new", timestamp_offset_seconds(300) });
        if (expect(lease_second_claim.size() == 1 && lease_second_claim[0].id == lease_job_id,
                   "expired job should be reclaimable") != 0 ||
            expect(store.complete_usage_commit_job(lease_job_id, "lease-new", "2026-06-23 12:07:05"),
                   "fresh lease holder should commit usage row") != 0 ||
            expect(store.usage_event_status_by_id(event_lease).value_or("") == "committed",
                   "usage row should commit after fresh lease completion") != 0) {
            return 1;
        }

        step("sink-direct-fallback");
        revlm::Config config;
        config.usage_finalize_queue_size = 1;
        config.usage_finalize_batch_size = 8;
        revlm::AsyncStreamUsageSink sink(store, config);
        constexpr long long event_sink_1 = 1010;
        constexpr long long event_sink_2 = 1011;
        const auto request_sink_1 = make_request(event_sink_1, 107, 207);
        auto request_sink_2 = make_request(event_sink_2, 9001, 9002);
        if (expect(sink.enqueue_or_commit_direct(make_job_input(event_sink_1, 107, 207)),
                   "sink should accept first queued item") != 0 ||
            expect(sink.enqueue_or_commit_direct(
                       revlm::UsageCommitJobInput{ event_sink_2, 108, 208, request_sink_2, true, false, false }),
                   "sink should fallback direct commit when full") != 0) {
            return 1;
        }
        const auto flushed = sink.flush();
        if (expect(flushed.size() == 1, "sink flush should persist queued job batch") != 0 ||
            expect(sink.fallback_sync_total() == 1, "sink should count direct fallback once") != 0 ||
            expect(store.count_usage_events_by_id(event_sink_2) == 1,
                   "direct fallback should write usage_event_id immediately") != 0) {
            return 1;
        }
        const auto sink_event_row =
            conn.query_rows("SELECT user_id,token_id FROM usage_events WHERE id=" + std::to_string(event_sink_2) +
                            " LIMIT 1");
        if (expect(!sink_event_row.empty() && sink_event_row[0].size() == 2 &&
                       sink_event_row[0][0].value_or("") == "108" && sink_event_row[0][1].value_or("") == "208",
                   "direct fallback should normalize outer user/token ids") != 0) {
            return 1;
        }
        if (expect(store.finalize_usage_commit_job(make_finalize(0, request_sink_1, "2026-06-23 12:07:00")),
                   "sink queued job should still finalize to ready") != 0) {
            return 1;
        }

        step("worker-drain");
        revlm::Config worker_cfg;
        worker_cfg.usage_commit_claim_size = 16;
        worker_cfg.usage_commit_workers = 2;
        worker_cfg.usage_commit_lease_ms = 20000;
        revlm::UsageCommitWorker worker(store, worker_cfg);
        const auto worker_metrics = worker.drain_once();
        if (expect(worker_metrics.claimed_total >= 1, "worker should claim at least one ready job") != 0 ||
            expect(worker_metrics.completed_total >= 1, "worker should complete at least one ready job") != 0 ||
            expect(store.count_usage_events_by_id(event_sink_1) == 1, "worker should commit queued sink job") != 0) {
            return 1;
        }

        step("runtime-tick");
        revlm::Config runtime_cfg;
        runtime_cfg.usage_finalize_queue_size = 2;
        runtime_cfg.usage_finalize_batch_size = 8;
        runtime_cfg.usage_commit_claim_size = 8;
        runtime_cfg.usage_commit_workers = 1;
        runtime_cfg.usage_commit_lease_ms = 20000;
        revlm::AsyncStreamUsageSink runtime_sink(store, runtime_cfg);
        revlm::UsageCommitWorker runtime_worker(store, runtime_cfg);
        constexpr long long event_runtime = 1012;
        const auto request_runtime = make_request(event_runtime, 112, 212);
        const long long runtime_job_id =
            store.create_usage_commit_job(make_job_input(event_runtime, 112, 212));
        if (expect(store.finalize_usage_commit_job(make_finalize(runtime_job_id, request_runtime, "2026-06-23 12:07:10")),
                   "runtime ready job should finalize") != 0) {
            return 1;
        }
        revlm::run_usage_commit_runtime_tick(store, runtime_sink, runtime_worker, timestamp_offset_seconds(-300));
        if (expect(store.count_usage_events_by_id(event_runtime) == 1,
                   "runtime tick should flush queue and commit ready job") != 0) {
            return 1;
        }

        step("runtime-wrapper");
        revlm::UsageFinalizeSink wrapped_sink(store, runtime_cfg);
        revlm::UsageCommitRuntime wrapped_runtime(store, wrapped_sink, runtime_cfg);
        constexpr long long event_wrapped = 1013;
        const auto request_wrapped = make_request(event_wrapped, 113, 213);
        if (expect(wrapped_sink.enqueue_or_commit_direct(make_job_input(event_wrapped, 113, 213)),
                   "wrapped sink should queue runtime job") != 0 ||
            expect(store.finalize_usage_commit_job(make_finalize(0, request_wrapped, "2026-06-23 12:07:20")),
                   "wrapped runtime job should finalize by usage_event_id") != 0) {
            return 1;
        }
        wrapped_runtime.tick(timestamp_offset_seconds(-300));
        const auto wrapped_metrics = wrapped_runtime.metrics();
        if (expect(store.count_usage_events_by_id(event_wrapped) == 1,
                   "wrapped runtime should commit finalized job") != 0 ||
            expect(wrapped_metrics.ticks >= 1, "wrapped runtime should record ticks") != 0 ||
            expect(wrapped_metrics.worker.completed_total >= 1, "wrapped runtime should publish completed metrics") !=
                0) {
            return 1;
        }

        step("auth-resolver");
        revlm::UserStore &user_store = revlm::UserStore::instance();
        user_store.reload(conn);
        revlm::TokenStore token_store(conn);
        const std::string auth_suffix = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        const size_t auth_tail_start = auth_suffix.size() > 8 ? auth_suffix.size() - 8 : 0;
        revlm::User auth_user("auth-" + auth_suffix + "@example.com", "auth" + auth_suffix.substr(auth_tail_start),
                              revlm::hash_password("secret-1"), "user");
        auth_user.status = 1;
        const long long auth_user_id = user_store.create_user(std::move(auth_user));
        const std::string raw_token = "sk-auth-" + auth_suffix;
        const long long auth_token_id = token_store.create_user_token(auth_user_id, std::nullopt, raw_token);
        if (expect(auth_token_id > 0, "auth test token should be created") != 0) {
            return 1;
        }
        runtime_cfg.db_dsn = dsn;
        revlm::AuthResolver resolver(runtime_cfg);
        const auto auth_first = resolver.resolve_token_auth(raw_token);
        if (expect(auth_first.has_value() && auth_first->token_id == auth_token_id,
                   "auth resolver should load token auth") != 0) {
            return 1;
        }
        token_store.revoke_user_token(auth_user_id, auth_token_id);
        const auto auth_revoked = resolver.resolve_token_auth(raw_token);
        if (expect(!auth_revoked.has_value(), "auth resolver should miss revoked token") != 0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "usage commit mysql smoke failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
