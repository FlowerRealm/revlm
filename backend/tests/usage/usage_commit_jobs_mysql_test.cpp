#include "auth/users.hpp"
#include "runtime/runtime_workers.hpp"
#include "server/tokens.hpp"
#include "store/migrations.hpp"
#include "usage/usage_commit_jobs.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
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

revlm::UsageCommitPayload make_payload(std::string request_id, long long user_id, long long token_id,
                                       bool direct_commit = false)
{
    revlm::UsageCommitPayload payload;
    payload.request_id = std::move(request_id);
    payload.user_id = user_id;
    payload.token_id = token_id;
    payload.occurred_at = "2026-06-23 12:00:00";
    payload.model = "gpt-5";
    payload.forwarded_model = "gpt-5";
    payload.upstream_response_model = "gpt-5";
    payload.requested_service_tier = "auto";
    payload.service_tier = "default";
    payload.input_tokens = 12;
    payload.cache_read_input_tokens = 2;
    payload.cache_creation_input_tokens = 3;
    payload.cache_creation_1h_input_tokens = 1;
    payload.output_tokens = 34;
    payload.committed_usd = "0.123456";
    payload.price_multiplier = "1.500000";
    payload.price_multiplier_group = "1.100000";
    payload.price_multiplier_payment = "1.200000";
    payload.price_multiplier_group_name = "default";
    payload.direct_commit = direct_commit;
    payload.finalize.endpoint = "/v1/responses";
    payload.finalize.method = "POST";
    payload.finalize.status_code = 200;
    payload.finalize.latency_ms = 128;
    payload.finalize.first_token_latency_ms = 45;
    payload.finalize.channel_id = 3;
    payload.finalize.is_stream = true;
    payload.finalize.request_bytes = 2048;
    payload.finalize.response_bytes = 4096;
    return payload;
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
        const auto payload_a = make_payload("req-a", 101, 201);
        const long long job_a =
            store.create_usage_commit_job(revlm::UsageCommitJobInput{ "req-a", 101, 201, payload_a });
        if (expect(job_a > 0, "create_usage_commit_job should return id") != 0) {
            return 1;
        }

        if (expect(store.finalize_usage_commit_job(revlm::UsageCommitFinalizeInput{
                       job_a, std::string{ revlm::usage_commit_job_state_streaming },
                       std::string{ revlm::usage_commit_job_state_ready }, payload_a, "2026-06-23 12:00:05" }),
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
            expect(store.count_usage_events_by_request_id("req-a") == 1, "complete should write one usage event") !=
                0 ||
            expect(store.usage_event_state_by_request_id("req-a").value_or("") == "committed",
                   "usage event should be committed") != 0) {
            return 1;
        }

        step("batch-fast-path");
        const auto payload_b = make_payload("req-b", 102, 202);
        const auto payload_c = make_payload("req-c", 103, 203);
        const auto fast_ids = store.create_usage_commit_jobs_fast({
            revlm::UsageCommitJobInput{ "req-b", 102, 202, payload_b },
            revlm::UsageCommitJobInput{ "req-c", 103, 203, payload_c },
        });
        if (expect(fast_ids.size() == 2, "create_usage_commit_jobs_fast should preserve batch size") != 0) {
            return 1;
        }
        if (expect(store.finalize_usage_commit_job(revlm::UsageCommitFinalizeInput{
                       0, std::string{ revlm::usage_commit_job_state_streaming },
                       std::string{ revlm::usage_commit_job_state_ready }, payload_b, "2026-06-23 12:03:00" }),
                   "finalize by request_id should work for req-b") != 0 ||
            expect(store.finalize_usage_commit_job(revlm::UsageCommitFinalizeInput{
                       0, std::string{ revlm::usage_commit_job_state_streaming },
                       std::string{ revlm::usage_commit_job_state_ready }, payload_c, "2026-06-23 12:03:01" }),
                   "finalize by request_id should work for req-c") != 0) {
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
            expect(store.count_usage_events_by_request_id("req-b") == 1, "req-b should be committed once") != 0 ||
            expect(store.count_usage_events_by_request_id("req-c") == 1, "req-c should be committed once") != 0) {
            return 1;
        }

        step("expired-processing-reclaim");
        const auto payload_reclaim_stale = make_payload("req-reclaim-stale", 109, 209);
        const auto payload_reclaim_ready = make_payload("req-reclaim-ready", 110, 210);
        const long long reclaim_stale_id = store.create_usage_commit_job(
            revlm::UsageCommitJobInput{ "req-reclaim-stale", 109, 209, payload_reclaim_stale });
        const long long reclaim_ready_id = store.create_usage_commit_job(
            revlm::UsageCommitJobInput{ "req-reclaim-ready", 110, 210, payload_reclaim_ready });
        if (expect(store.finalize_usage_commit_job(revlm::UsageCommitFinalizeInput{
                       reclaim_stale_id, std::string{ revlm::usage_commit_job_state_streaming },
                       std::string{ revlm::usage_commit_job_state_ready }, payload_reclaim_stale,
                       "2026-06-23 12:04:00" }),
                   "reclaim stale finalize should work") != 0 ||
            expect(store.finalize_usage_commit_job(revlm::UsageCommitFinalizeInput{
                       reclaim_ready_id, std::string{ revlm::usage_commit_job_state_streaming },
                       std::string{ revlm::usage_commit_job_state_ready }, payload_reclaim_ready,
                       "2026-06-23 12:04:01" }),
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
        if (expect(store.count_usage_events_by_request_id("req-reclaim-stale") == 1,
                   "reclaimed stale job should still commit once") != 0 ||
            expect(store.count_usage_events_by_request_id("req-reclaim-ready") == 1,
                   "ready job should commit alongside reclaimed stale job") != 0) {
            return 1;
        }

        step("stale-abort");
        const auto payload_stale = make_payload("req-stale", 104, 204);
        const long long stale_id =
            store.create_usage_commit_job(revlm::UsageCommitJobInput{ "req-stale", 104, 204, payload_stale });
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
        auto payload_retry = make_payload("req-retry", 105, 205);
        payload_retry.direct_commit = true;
        payload_retry.retryable = true;
        payload_retry.committed_usd = "oops";
        const long long retry_id =
            store.create_usage_commit_job(revlm::UsageCommitJobInput{ "req-retry", 105, 205, payload_retry });
        if (expect(store.finalize_usage_commit_job(revlm::UsageCommitFinalizeInput{
                       retry_id, std::string{ revlm::usage_commit_job_state_streaming },
                       std::string{ revlm::usage_commit_job_state_ready }, payload_retry, "2026-06-23 12:05:00" }),
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
        auto payload_dead = make_payload("req-dead", 106, 206);
        payload_dead.direct_commit = true;
        payload_dead.retryable = false;
        payload_dead.committed_usd = "oops";
        const long long dead_id =
            store.create_usage_commit_job(revlm::UsageCommitJobInput{ "req-dead", 106, 206, payload_dead });
        if (expect(store.finalize_usage_commit_job(revlm::UsageCommitFinalizeInput{
                       dead_id, std::string{ revlm::usage_commit_job_state_streaming },
                       std::string{ revlm::usage_commit_job_state_ready }, payload_dead, "2026-06-23 12:06:00" }),
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

        step("stale-lease-prepared-row");
        conn.exec(
            "INSERT INTO usage_events(time,request_id,user_id,token_id,state,committed_usd,created_at,updated_at) "
            "VALUES('2026-06-23 12:06:50','req-prepared-stale',111,211,'pending',0.000000,"
            "'2026-06-23 12:06:50','2026-06-23 12:06:50')");
        auto payload_prepared = make_payload("req-prepared-stale", 111, 211);
        payload_prepared.prepared_usage_event_id = static_cast<long long>(conn.last_insert_id());
        const long long prepared_job_id = store.create_usage_commit_job(
            revlm::UsageCommitJobInput{ "req-prepared-stale", 111, 211, payload_prepared });
        if (expect(store.finalize_usage_commit_job(revlm::UsageCommitFinalizeInput{
                       prepared_job_id, std::string{ revlm::usage_commit_job_state_streaming },
                       std::string{ revlm::usage_commit_job_state_ready }, payload_prepared, "2026-06-23 12:06:55" }),
                   "prepared job finalize should work") != 0) {
            return 1;
        }
        const auto prepared_first_claim = store.claim_ready_usage_commit_jobs(
            revlm::UsageCommitClaimInput{ 1, "lease-prepared-old", timestamp_offset_seconds(300) });
        if (expect(prepared_first_claim.size() == 1 && prepared_first_claim[0].id == prepared_job_id,
                   "prepared job should be claimable") != 0) {
            return 1;
        }
        conn.exec("UPDATE usage_commit_jobs SET lease_until='2000-01-01 00:00:00' WHERE id=" +
                  std::to_string(prepared_job_id));
        if (expect(!store.complete_usage_commit_job(prepared_job_id, "lease-prepared-old", "2026-06-23 12:07:00"),
                   "stale lease holder must not commit prepared usage row") != 0 ||
            expect(store.usage_event_state_by_request_id("req-prepared-stale").value_or("") == "pending",
                   "prepared usage row should stay pending after stale lease rejection") != 0) {
            return 1;
        }
        const auto prepared_second_claim = store.claim_ready_usage_commit_jobs(
            revlm::UsageCommitClaimInput{ 1, "lease-prepared-new", timestamp_offset_seconds(300) });
        if (expect(prepared_second_claim.size() == 1 && prepared_second_claim[0].id == prepared_job_id,
                   "expired prepared job should be reclaimable") != 0 ||
            expect(store.complete_usage_commit_job(prepared_job_id, "lease-prepared-new", "2026-06-23 12:07:05"),
                   "fresh lease holder should commit prepared usage row") != 0 ||
            expect(store.usage_event_state_by_request_id("req-prepared-stale").value_or("") == "committed",
                   "prepared usage row should commit after fresh lease completion") != 0) {
            return 1;
        }

        step("sink-direct-fallback");
        revlm::Config config;
        config.usage_finalize_queue_size = 1;
        config.usage_finalize_batch_size = 8;
        revlm::AsyncStreamUsageSink sink(store, config);
        const auto payload_sink_1 = make_payload("req-sink-1", 107, 207);
        auto payload_sink_2 = make_payload("req-sink-2-inner", 9001, 9002, true);
        if (expect(sink.enqueue_or_commit_direct(revlm::UsageCommitJobInput{ "req-sink-1", 107, 207, payload_sink_1 }),
                   "sink should accept first queued item") != 0 ||
            expect(sink.enqueue_or_commit_direct(revlm::UsageCommitJobInput{ "req-sink-2", 108, 208, payload_sink_2 }),
                   "sink should fallback direct commit when full") != 0) {
            return 1;
        }
        const auto flushed = sink.flush();
        if (expect(flushed.size() == 1, "sink flush should persist queued job batch") != 0 ||
            expect(sink.fallback_sync_total() == 1, "sink should count direct fallback once") != 0 ||
            expect(store.count_usage_events_by_request_id("req-sink-2") == 1,
                   "direct fallback should write normalized request id immediately") != 0 ||
            expect(store.count_usage_events_by_request_id("req-sink-2-inner") == 0,
                   "direct fallback must not leak payload request id over input request id") != 0) {
            return 1;
        }
        const auto sink_event_row =
            conn.query_rows("SELECT user_id,token_id FROM usage_events WHERE request_id='req-sink-2' LIMIT 1");
        if (expect(!sink_event_row.empty() && sink_event_row[0].size() == 2 &&
                       sink_event_row[0][0].value_or("") == "108" && sink_event_row[0][1].value_or("") == "208",
                   "direct fallback should normalize outer user/token ids") != 0) {
            return 1;
        }
        if (expect(store.finalize_usage_commit_job(revlm::UsageCommitFinalizeInput{
                       0, std::string{ revlm::usage_commit_job_state_streaming },
                       std::string{ revlm::usage_commit_job_state_ready }, payload_sink_1, "2026-06-23 12:07:00" }),
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
            expect(store.count_usage_events_by_request_id("req-sink-1") == 1, "worker should commit queued sink job") !=
                0) {
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
        const auto payload_runtime = make_payload("req-runtime", 112, 212);
        const long long runtime_job_id =
            store.create_usage_commit_job(revlm::UsageCommitJobInput{ "req-runtime", 112, 212, payload_runtime });
        if (expect(store.finalize_usage_commit_job(revlm::UsageCommitFinalizeInput{
                       runtime_job_id, std::string{ revlm::usage_commit_job_state_streaming },
                       std::string{ revlm::usage_commit_job_state_ready }, payload_runtime, "2026-06-23 12:07:10" }),
                   "runtime ready job should finalize") != 0) {
            return 1;
        }
        revlm::run_usage_commit_runtime_tick(store, runtime_sink, runtime_worker, timestamp_offset_seconds(-300));
        if (expect(store.count_usage_events_by_request_id("req-runtime") == 1,
                   "runtime tick should flush queue and commit ready job") != 0) {
            return 1;
        }

        step("runtime-wrapper");
        revlm::UsageFinalizeSink wrapped_sink(store, runtime_cfg);
        revlm::UsageCommitRuntime wrapped_runtime(store, wrapped_sink, runtime_cfg);
        const auto payload_wrapped = make_payload("req-runtime-wrapper", 113, 213);
        if (expect(wrapped_sink.enqueue_or_commit_direct(
                       revlm::UsageCommitJobInput{ "req-runtime-wrapper", 113, 213, payload_wrapped }),
                   "wrapped sink should queue runtime job") != 0 ||
            expect(store.finalize_usage_commit_job(revlm::UsageCommitFinalizeInput{
                       0, std::string{ revlm::usage_commit_job_state_streaming },
                       std::string{ revlm::usage_commit_job_state_ready }, payload_wrapped, "2026-06-23 12:07:20" }),
                   "wrapped runtime job should finalize by request id") != 0) {
            return 1;
        }
        wrapped_runtime.tick(timestamp_offset_seconds(-300));
        const auto wrapped_metrics = wrapped_runtime.metrics();
        if (expect(store.count_usage_events_by_request_id("req-runtime-wrapper") == 1,
                   "wrapped runtime should commit finalized job") != 0 ||
            expect(wrapped_metrics.ticks >= 1, "wrapped runtime should record ticks") != 0 ||
            expect(wrapped_metrics.worker.completed_total >= 1, "wrapped runtime should publish completed metrics") !=
                0) {
            return 1;
        }

        step("auth-resolver");
        revlm::UserStore user_store(conn);
        revlm::TokenStore token_store(conn);
        const std::string auth_suffix = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        const size_t auth_tail_start = auth_suffix.size() > 8 ? auth_suffix.size() - 8 : 0;
        const long long auth_user_id = user_store.create_user(
            revlm::User("auth-" + auth_suffix + "@example.com", "auth" + auth_suffix.substr(auth_tail_start),
                        revlm::hash_password("secret-1"), "user"));
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
