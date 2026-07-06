#include "store/migrations.hpp"
#include "store/mysql.hpp"
#include "usage/usage_aggregation.hpp"

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

void exec_many(revlm::MysqlConnection &conn, const std::vector<std::string> &sqls)
{
    for (const std::string &sql : sqls) {
        conn.exec(sql);
    }
}

} // namespace

int main()
{
    const char *dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        std::cout << "REVLM_TEST_MYSQL_DSN not set; skipping usage aggregation MySQL test\n";
        return 0;
    }

    try {
        (void)revlm::apply_migrations(dsn, "internal/store/migrations", "", 30);
        revlm::MysqlConnection conn(dsn, revlm::mysql_client_multi_statements);

        exec_many(conn, {
                            "DELETE FROM usage_minute_stats_backfilled",
                            "DELETE FROM usage_hourly_stats_backfilled",
                            "DELETE FROM usage_daily_stats_backfilled",
                            "DELETE FROM usage_minute_model_stats",
                            "DELETE FROM usage_hourly_model_stats",
                            "DELETE FROM usage_daily_model_stats",
                            "DELETE FROM usage_minute_scope_stats",
                            "DELETE FROM usage_hourly_scope_stats",
                            "DELETE FROM usage_daily_scope_stats",
                            "DELETE FROM usage_minute_stats",
                            "DELETE FROM usage_hourly_stats",
                            "DELETE FROM usage_daily_stats",
                            "DELETE FROM usage_events",
                        });

        exec_many(
            conn,
            {
                "INSERT INTO usage_events "
                "(time,request_id,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                "user_id,token_id,channel_id,"
                "state,model,input_tokens,cache_read_input_tokens,cache_creation_input_tokens,"
                "cache_creation_1h_input_tokens,output_tokens,committed_usd,price_multiplier,"
                "price_multiplier_group,price_multiplier_payment,price_multiplier_group_name,"
                "is_stream,request_bytes,response_bytes,created_at,updated_at) VALUES "
                "('2026-06-20 00:00:20','req-d016-1','/v1/responses','POST',200,180,30,"
                "101,201,301,'committed','gpt-5.5',100,20,7,2,40,1.250000,1.0,1.0,1.0,'g/a',0,1000,2000,"
                "'2026-06-20 00:00:20','2026-06-20 00:00:20')",
                "INSERT INTO usage_events "
                "(time,request_id,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                "user_id,token_id,channel_id,"
                "state,model,input_tokens,cache_read_input_tokens,cache_creation_input_tokens,"
                "cache_creation_1h_input_tokens,output_tokens,committed_usd,price_multiplier,"
                "price_multiplier_group,price_multiplier_payment,price_multiplier_group_name,"
                "is_stream,request_bytes,response_bytes,created_at,updated_at) VALUES "
                "('2026-06-20 00:30:00','req-d016-2','/v1/responses','POST',500,90,0,"
                "101,201,301,'committed','gpt-5.5',10,0,0,0,5,0.500000,1.0,1.0,1.0,'g/a',0,500,800,"
                "'2026-06-20 00:30:00','2026-06-20 00:30:00')",
                "INSERT INTO usage_events "
                "(time,request_id,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                "user_id,token_id,channel_id,"
                "state,model,input_tokens,cache_read_input_tokens,cache_creation_input_tokens,"
                "cache_creation_1h_input_tokens,output_tokens,committed_usd,price_multiplier,"
                "price_multiplier_group,price_multiplier_payment,price_multiplier_group_name,"
                "is_stream,request_bytes,response_bytes,created_at,updated_at) VALUES "
                "('2026-06-20 01:10:00','req-d016-3','/v1/chat/completions','POST',200,160,50,"
                "101,202,302,'committed','claude-opus-4-8',80,15,9,4,70,2.000000,1.0,1.0,1.0,'g/b',1,1200,2500,"
                "'2026-06-20 01:10:00','2026-06-20 01:10:00')",
                "INSERT INTO usage_events "
                "(time,request_id,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                "user_id,token_id,channel_id,"
                "state,model,input_tokens,cache_read_input_tokens,cache_creation_input_tokens,"
                "cache_creation_1h_input_tokens,output_tokens,committed_usd,price_multiplier,"
                "price_multiplier_group,price_multiplier_payment,price_multiplier_group_name,"
                "is_stream,request_bytes,response_bytes,created_at,updated_at) VALUES "
                "('2026-06-21 00:00:10','req-d016-4','/v1/messages','POST',200,70,20,"
                "102,203,301,'committed','claude-haiku-4-5-20251001',50,5,3,1,20,0.750000,1.0,1.0,1.0,'g/a',0,700,900,"
                "'2026-06-21 00:00:10','2026-06-21 00:00:10')",
                "INSERT INTO usage_events "
                "(time,request_id,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                "user_id,token_id,channel_id,"
                "state,model,input_tokens,cache_read_input_tokens,cache_creation_input_tokens,"
                "cache_creation_1h_input_tokens,output_tokens,committed_usd,price_multiplier,"
                "price_multiplier_group,price_multiplier_payment,price_multiplier_group_name,"
                "is_stream,request_bytes,response_bytes,created_at,updated_at) VALUES "
                "('2026-06-20 00:01:00','req-d016-null-rollup','/v1/responses','POST',200,45,15,"
                "103,204,NULL,'committed','gpt-5.5',7,1,0,0,3,0.125000,1.0,1.0,1.0,'g/c',0,321,654,"
                "'2026-06-20 00:01:00','2026-06-20 00:01:00')",
                "INSERT INTO usage_events "
                "(time,request_id,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                "user_id,token_id,channel_id,"
                "state,model,input_tokens,cache_read_input_tokens,cache_creation_input_tokens,"
                "cache_creation_1h_input_tokens,output_tokens,committed_usd,price_multiplier,"
                "price_multiplier_group,price_multiplier_payment,price_multiplier_group_name,"
                "is_stream,request_bytes,response_bytes,created_at,updated_at) VALUES "
                "('2026-06-20 00:00:20','req-d016-null-raw','/v1/responses','POST',500,35,0,"
                "103,204,NULL,'committed','gpt-5.5',11,2,0,0,5,0.250000,1.0,1.0,1.0,'g/c',0,222,333,"
                "'2026-06-20 00:00:20','2026-06-20 00:00:20')",
                "INSERT INTO usage_events "
                "(time,request_id,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                "user_id,token_id,channel_id,"
                "state,model,input_tokens,cache_read_input_tokens,cache_creation_input_tokens,"
                "cache_creation_1h_input_tokens,output_tokens,committed_usd,price_multiplier,"
                "price_multiplier_group,price_multiplier_payment,price_multiplier_group_name,"
                "is_stream,request_bytes,response_bytes,created_at,updated_at) VALUES "
                "('2026-06-20 00:05:00','req-d016-pending','/v1/responses','POST',200,10,0,"
                "101,201,301,'pending','gpt-5.5',999,999,999,0,999,99.000000,1.0,1.0,1.0,'g/a',0,1,1,"
                "'2026-06-20 00:05:00','2026-06-20 00:05:00')",
            });

        revlm::UsageAggregationStore store(conn);

        const auto split = store.split_query_range("2026-06-20 00:00:20", "2026-06-21 00:00:10");
        if (expect(split.size() == 4, "range should split into raw/minute/hour/raw segments") != 0 ||
            expect(revlm::usage_range_grain_name(split[0].grain) == "raw" &&
                       revlm::usage_range_grain_name(split[1].grain) == "minute" &&
                       revlm::usage_range_grain_name(split[2].grain) == "hour" &&
                       revlm::usage_range_grain_name(split[3].grain) == "raw",
                   "range split grains should be raw/minute/hour/raw") != 0) {
            return 1;
        }

        const auto split_day = store.split_query_range("2026-06-20 00:00:00", "2026-06-22 00:00:00");
        if (expect(split_day.size() == 1, "full-day aligned range should collapse to one day segment") != 0 ||
            expect(revlm::usage_range_grain_name(split_day[0].grain) == "day" &&
                       split_day[0].start_utc == "2026-06-20 00:00:00" && split_day[0].end_utc == "2026-06-22 00:00:00",
                   "aligned full-day range should use day grain") != 0) {
            return 1;
        }

        const auto mixed_zero_split = store.split_query_range("2026-06-20 00:00:20", "2026-06-20 00:01:20");
        if (expect(mixed_zero_split.size() == 1, "sub-minute tail should stay on raw grain only") != 0 ||
            expect(revlm::usage_range_grain_name(mixed_zero_split[0].grain) == "raw" &&
                       mixed_zero_split[0].start_utc == "2026-06-20 00:00:20" &&
                       mixed_zero_split[0].end_utc == "2026-06-20 00:01:20",
                   "sub-minute range should not use partial minute rollup") != 0) {
            return 1;
        }

        revlm::UsagePrimaryFilter user101_filter;
        user101_filter.user_id = 101;
        const auto primary = store.sum_primary("2026-06-20 00:00:20", "2026-06-21 00:00:10", user101_filter);
        if (expect(primary.requests == 3, "primary sum should count only committed rows for user 101") != 0 ||
            expect(primary.input_tokens == 190, "primary sum input tokens mismatch") != 0 ||
            expect(primary.cache_read_input_tokens == 35, "primary sum cache_read mismatch") != 0 ||
            expect(primary.cache_creation_input_tokens == 16, "primary sum cache_creation mismatch") != 0 ||
            expect(primary.output_tokens == 115, "primary sum output tokens mismatch") != 0 ||
            expect(primary.committed_usd_micros == 3750000, "primary sum committed usd mismatch") != 0 ||
            expect(primary.first_token_samples == 2, "primary sum first token samples mismatch") != 0 ||
            expect(primary.first_token_latency_sum == 80, "primary sum first token latency mismatch") != 0) {
            return 1;
        }

        const auto scope = store.sum_scope("2026-06-20 00:00:20", "2026-06-21 00:00:10", "channel", 301);
        if (expect(scope.requests == 2, "channel scope should aggregate matching channel rows") != 0 ||
            expect(scope.input_tokens == 110, "channel scope input mismatch") != 0 ||
            expect(scope.output_tokens == 45, "channel scope output mismatch") != 0) {
            return 1;
        }

        const auto model = store.sum_model("2026-06-20 00:00:20", "2026-06-21 00:00:10", 101, "gpt-5.5");
        if (expect(model.requests == 2, "model sum should aggregate exact user/model rows") != 0 ||
            expect(model.input_tokens == 110, "model sum input mismatch") != 0 ||
            expect(model.output_tokens == 45, "model sum output mismatch") != 0) {
            return 1;
        }

        revlm::UsagePrimaryFilter null_channel_filter;
        null_channel_filter.channel_id = 0;
        const auto null_primary = store.sum_primary("2026-06-20 00:00:20", "2026-06-20 00:01:20", null_channel_filter);
        if (expect(null_primary.requests == 2, "channel 0 primary bucket should include raw and rollup NULL rows") !=
                0 ||
            expect(null_primary.input_tokens == 18, "channel 0 primary input mismatch across raw and rollup") != 0 ||
            expect(null_primary.output_tokens == 8, "channel 0 primary output mismatch across raw and rollup") != 0) {
            return 1;
        }

        const auto null_scope = store.sum_scope("2026-06-20 00:00:20", "2026-06-20 00:01:20", "channel", 0);
        if (expect(null_scope.requests == 2, "channel scope 0 should include raw and rollup NULL rows") != 0 ||
            expect(null_scope.input_tokens == 18, "channel scope 0 input mismatch across raw and rollup") != 0 ||
            expect(null_scope.output_tokens == 8, "channel scope 0 output mismatch across raw and rollup") != 0) {
            return 1;
        }

        const auto markers_before = conn.query_rows("SELECT (SELECT COUNT(*) FROM usage_daily_stats_backfilled), "
                                                    "(SELECT COUNT(*) FROM usage_hourly_stats_backfilled), "
                                                    "(SELECT COUNT(*) FROM usage_minute_stats_backfilled)");
        if (expect(!markers_before.empty() && markers_before[0][0].value_or("0") == "0" &&
                       markers_before[0][1].value_or("0") == "23" && markers_before[0][2].value_or("0") == "59",
                   "backfill markers should match actual hour/minute coverage for the split range") != 0) {
            return 1;
        }

        const auto day_aligned_primary = store.sum_primary("2026-06-20 00:00:00", "2026-06-22 00:00:00");
        if (expect(day_aligned_primary.requests == 6, "day-aligned range should still aggregate all committed rows") !=
            0) {
            return 1;
        }
        const auto day_markers_after = conn.query_rows("SELECT COUNT(*) FROM usage_daily_stats_backfilled");
        if (expect(!day_markers_after.empty() && day_markers_after[0][0].value_or("0") == "2",
                   "day-aligned range should backfill day markers for both days") != 0) {
            return 1;
        }

        revlm::UsagePrimaryFilter rebuilt_filter;
        rebuilt_filter.user_id = 101;
        rebuilt_filter.token_id = 201;
        const auto aligned_before = store.sum_primary("2026-06-20 00:00:00", "2026-06-20 00:01:00", rebuilt_filter);
        if (expect(aligned_before.requests == 1, "aligned minute query should build initial minute bucket") != 0) {
            return 1;
        }
        const std::uint64_t epoch_before = store.coverage_epoch();
        const auto minute_bucket_before = conn.query_rows(
            "SELECT COALESCE(SUM(requests),0) FROM usage_minute_stats WHERE stat_minute='2026-06-20 00:00:00'");
        exec_many(conn, {
                            "INSERT INTO usage_events "
                            "(time,request_id,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                            "user_id,token_id,channel_id,"
                            "state,model,input_tokens,cache_read_input_tokens,cache_creation_input_tokens,"
                            "cache_creation_1h_input_tokens,output_tokens,committed_usd,price_multiplier,"
                            "price_multiplier_group,price_multiplier_payment,price_multiplier_group_name,"
                            "is_stream,request_bytes,response_bytes,created_at,updated_at) VALUES "
                            "('2026-06-20 00:00:40','req-d016-5','/v1/responses','POST',200,40,10,"
                            "101,201,301,'committed','gpt-5.5',30,2,1,0,10,0.250000,1.0,1.0,1.0,'g/a',0,300,500,"
                            "'2026-06-20 00:00:40','2026-06-20 00:00:40')",
                        });
        store.invalidate_coverage_for_time("2026-06-20 00:00:40");
        if (expect(store.coverage_epoch() == epoch_before + 1, "coverage epoch should bump on invalidation") != 0) {
            return 1;
        }
        const auto rebuilt = store.sum_primary("2026-06-20 00:00:00", "2026-06-20 00:01:00", rebuilt_filter);
        if (expect(rebuilt.requests == 2, "rebuilt minute coverage should include inserted row") != 0 ||
            expect(rebuilt.input_tokens == 130, "rebuilt minute coverage input mismatch") != 0 ||
            expect(rebuilt.output_tokens == 50, "rebuilt minute coverage output mismatch") != 0) {
            return 1;
        }

        const auto minute_bucket_after = conn.query_rows(
            "SELECT COALESCE(SUM(requests),0) FROM usage_minute_stats WHERE stat_minute='2026-06-20 00:00:00'");
        if (expect(!minute_bucket_before.empty() && !minute_bucket_after.empty() &&
                       minute_bucket_before[0][0].value_or("0") == "2" &&
                       minute_bucket_after[0][0].value_or("0") == "3",
                   "minute rollup bucket should be rebuilt after invalidation") != 0) {
            return 1;
        }

        const auto hour_before = store.sum_primary("2026-06-20 01:00:00", "2026-06-20 02:00:00", user101_filter);
        if (expect(hour_before.requests == 1, "hour-aligned query should see initial one-row bucket") != 0) {
            return 1;
        }
        exec_many(conn, {
                            "INSERT INTO usage_events "
                            "(time,request_id,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
                            "user_id,token_id,channel_id,"
                            "state,model,input_tokens,cache_read_input_tokens,cache_creation_input_tokens,"
                            "cache_creation_1h_input_tokens,output_tokens,committed_usd,price_multiplier,"
                            "price_multiplier_group,price_multiplier_payment,price_multiplier_group_name,"
                            "is_stream,request_bytes,response_bytes,created_at,updated_at) VALUES "
                            "('2026-06-20 01:20:00','req-d016-6','/v1/chat/completions','POST',200,55,12,"
                            "101,202,302,'committed','claude-opus-4-8',11,1,0,0,8,0.125000,1.0,1.0,1.0,'g/b',0,111,222,"
                            "'2026-06-20 01:20:00','2026-06-20 01:20:00')",
                        });
        const auto hour_after = store.sum_primary("2026-06-20 01:00:00", "2026-06-20 02:00:00", user101_filter);
        if (expect(hour_after.requests == 2,
                   "fresh committed usage should refresh hour coverage without manual invalidation") != 0 ||
            expect(hour_after.input_tokens == 91, "hour coverage refresh should include new input tokens") != 0 ||
            expect(hour_after.output_tokens == 78, "hour coverage refresh should include new output tokens") != 0) {
            return 1;
        }

        bool rejected_deleted_scope = false;
        try {
            (void)store.sum_scope("2026-06-20 00:00:00", "2026-06-20 01:00:00", "subscription", 1);
        } catch (const std::invalid_argument &) {
            rejected_deleted_scope = true;
        }
        if (expect(rejected_deleted_scope, "deleted scope types should be rejected consistently") != 0) {
            return 1;
        }
    } catch (const std::exception &err) {
        std::cerr << "usage aggregation MySQL test failed: " << err.what() << '\n';
        return 1;
    }

    return 0;
}
