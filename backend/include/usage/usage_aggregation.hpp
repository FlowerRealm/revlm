#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "store/mysql.hpp"

namespace revlm
{

enum class UsageRangeGrain {
    raw,
    minute,
    hour,
    day,
};

struct UsageRangeSegment {
    UsageRangeGrain grain = UsageRangeGrain::raw;
    std::string start_utc;
    std::string end_utc;
};

struct UsageTotals {
    long long requests = 0;
    long long input_tokens = 0;
    long long cache_read_input_tokens = 0;
    long long cache_creation_input_tokens = 0;
    long long output_tokens = 0;
    long long committed_usd_micros = 0;
    long long first_token_samples = 0;
    long long first_token_latency_sum = 0;
    long long output_tokens_for_tps = 0;
    long long decode_latency_sum = 0;
};

struct UsagePrimaryFilter {
    std::optional<long long> user_id;
    std::optional<long long> token_id;
    std::optional<long long> channel_id;
};

std::string usage_range_grain_name(UsageRangeGrain grain);

class UsageAggregationStore {
public:
    struct CoverageCacheEntry {
        std::string key;
        std::uint64_t epoch = 0;
        std::optional<long long> source_watermark;
    };

    explicit UsageAggregationStore(MysqlConnection &conn);

    std::uint64_t coverage_epoch() const;
    void invalidate_coverage_for_time(std::string_view time_utc);

    std::vector<UsageRangeSegment> split_query_range(std::string_view start_utc, std::string_view end_utc) const;

    void ensure_daily_coverage(std::string_view start_utc, std::string_view end_utc);
    void ensure_hourly_coverage(std::string_view start_utc, std::string_view end_utc);
    void ensure_minute_coverage(std::string_view start_utc, std::string_view end_utc);

    UsageTotals sum_primary(std::string_view start_utc, std::string_view end_utc,
                            const UsagePrimaryFilter &filter = {});
    UsageTotals sum_scope(std::string_view start_utc, std::string_view end_utc, std::string_view scope_type,
                          long long scope_id);
    UsageTotals sum_model(std::string_view start_utc, std::string_view end_utc, long long user_id,
                          std::string_view model);

private:
    MysqlConnection &conn_;
    std::uint64_t coverage_epoch_ = 0;
    std::vector<CoverageCacheEntry> coverage_cache_;
};

} // namespace revlm
