#include "usage/usage_commit_jobs.hpp"

#include "models/models.hpp"
#include "models/quota.hpp"
#include "request/request.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <random>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

std::string json_quote(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (char ch : value) {
        switch (ch) {
        case '\\':
        case '"':
            out.push_back('\\');
            out.push_back(ch);
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    out.push_back('"');
    return out;
}

std::string json_string_or_null(std::string_view value)
{
    return value.empty() ? "null" : json_quote(value);
}

std::string json_double(double value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", value);
    return std::string{ buf };
}

bool debit_request_balance(MysqlConnection &conn, const Request &request, bool balance_debited)
{
    if (balance_debited) {
        return true;
    }

    Request req = request;
    try {
        Quota(conn).charge(req);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

UsageCommitJobInput normalize_job_input(const UsageCommitJobInput &input)
{
    UsageCommitJobInput normalized = input;
    assert(normalized.user_id > 0 && "internal: user_id must be positive");
    if (normalized.user_id <= 0) {
        return {};
    }
    assert(normalized.token_id > 0 && "internal: token_id must be positive");
    if (normalized.token_id <= 0) {
        return {};
    }
    assert(normalized.usage_event_id > 0 && "internal: usage_event_id must be positive");
    if (normalized.usage_event_id <= 0) {
        return {};
    }
    if (normalized.request.id <= 0) {
        normalized.request.id = normalized.usage_event_id;
    }
    normalized.request.user_id = normalized.user_id;
    normalized.request.token_id = normalized.token_id;
    return normalized;
}

std::string request_to_payload_json(const Request &request, bool direct_commit, bool balance_debited, bool retryable)
{
    std::ostringstream out;
    out << "{";
    out << "\"id\":" << request.id;
    out << ",\"user_id\":" << request.user_id;
    out << ",\"token_id\":" << request.token_id;
    out << ",\"occurred_at\":" << json_string_or_null(request.time);
    out << ",\"time\":" << json_string_or_null(request.time);
    out << ",\"model\":" << json_string_or_null(request.model.name);
    out << ",\"service_tier\":" << json_string_or_null(request.service_tier);
    out << ",\"input_tokens\":" << request.input_tokens;
    out << ",\"cache_read_tokens\":" << request.cache_read_tokens;
    out << ",\"cache_creation_5m_tokens\":" << request.cache_creation_5m_tokens;
    out << ",\"cache_creation_1h_tokens\":" << request.cache_creation_1h_tokens;
    out << ",\"output_tokens\":" << request.output_tokens;
    out << ",\"tier_multiplier\":" << json_double(request.tier_multiplier);
    out << ",\"channel_multiplier\":" << json_double(request.channel_multiplier);
    out << ",\"endpoint\":" << json_string_or_null(request.endpoint);
    out << ",\"method\":" << json_string_or_null(request.method);
    out << ",\"status_code\":" << request.status_code;
    out << ",\"latency_ms\":" << request.latency_ms;
    out << ",\"first_token_latency_ms\":" << request.first_token_latency_ms;
    out << ",\"error_class\":" << json_string_or_null(request.error_class);
    out << ",\"error_message\":" << json_string_or_null(request.error_message);
    out << ",\"channel_id\":" << request.channel_id;
    out << ",\"is_stream\":" << (request.is_stream ? "true" : "false");
    out << ",\"statue\":" << (request.statue ? "true" : "false");
    out << ",\"direct_commit\":" << (direct_commit ? "true" : "false");
    out << ",\"balance_debited\":" << (balance_debited ? "true" : "false");
    out << ",\"retryable\":" << (retryable ? "true" : "false");
    out << "}";
    return out.str();
}

std::optional<std::string> json_extract_string(MysqlConnection &conn, long long usage_event_id, std::string_view path)
{
    return conn.query_one("SELECT JSON_UNQUOTE(JSON_EXTRACT(payload_json, " + conn.quote(path) +
                          ")) FROM usage_commit_jobs WHERE usage_event_id=" + std::to_string(usage_event_id) +
                          " LIMIT 1");
}

std::optional<long long> json_extract_int(MysqlConnection &conn, long long usage_event_id, std::string_view path)
{
    const auto raw = conn.query_one("SELECT JSON_UNQUOTE(JSON_EXTRACT(payload_json, " + conn.quote(path) +
                                    ")) FROM usage_commit_jobs WHERE usage_event_id=" +
                                    std::to_string(usage_event_id) + " LIMIT 1");
    if (!raw.has_value() || raw->empty() || *raw == "null") {
        return std::nullopt;
    }
    return std::stoll(*raw);
}

std::optional<double> json_extract_double(MysqlConnection &conn, long long usage_event_id, std::string_view path)
{
    const auto raw = conn.query_one("SELECT JSON_UNQUOTE(JSON_EXTRACT(payload_json, " + conn.quote(path) +
                                    ")) FROM usage_commit_jobs WHERE usage_event_id=" +
                                    std::to_string(usage_event_id) + " LIMIT 1");
    if (!raw.has_value() || raw->empty() || *raw == "null") {
        return std::nullopt;
    }
    try {
        return std::stod(*raw);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

std::optional<bool> json_extract_bool(MysqlConnection &conn, long long usage_event_id, std::string_view path)
{
    const auto raw =
        conn.query_one("SELECT JSON_EXTRACT(payload_json, " + conn.quote(path) +
                       ") FROM usage_commit_jobs WHERE usage_event_id=" + std::to_string(usage_event_id) + " LIMIT 1");
    if (!raw.has_value() || raw->empty() || *raw == "null") {
        return std::nullopt;
    }
    return *raw == "true" || *raw == "1";
}

void hydrate_model_by_name(Request &request, std::string_view model_name)
{
    const std::string name = trim_ascii(model_name);
    if (name.empty()) {
        return;
    }
    const std::vector<Model> &models = ModelManager::instance().models();
    const auto it = std::ranges::find(models, name, &Model::name);
    if (it != models.end()) {
        request.model = *it;
    } else {
        request.model.name = name;
    }
}

Request load_request_by_usage_event_id(MysqlConnection &conn, long long usage_event_id)
{
    Request request;
    request.id = json_extract_int(conn, usage_event_id, "$.id").value_or(usage_event_id);
    request.user_id = json_extract_int(conn, usage_event_id, "$.user_id").value_or(0);
    request.token_id = json_extract_int(conn, usage_event_id, "$.token_id").value_or(0);
    const auto occurred_at = json_extract_string(conn, usage_event_id, "$.occurred_at");
    const auto time = json_extract_string(conn, usage_event_id, "$.time");
    if (occurred_at.has_value() && !trim_ascii(*occurred_at).empty()) {
        request.time = trim_ascii(*occurred_at);
    } else if (time.has_value()) {
        request.time = trim_ascii(*time);
    }
    const auto model = json_extract_string(conn, usage_event_id, "$.model");
    if (model.has_value()) {
        hydrate_model_by_name(request, *model);
    }
    request.service_tier = json_extract_string(conn, usage_event_id, "$.service_tier").value_or("");
    request.input_tokens = static_cast<int>(json_extract_int(conn, usage_event_id, "$.input_tokens").value_or(0));
    request.cache_read_tokens =
        static_cast<int>(json_extract_int(conn, usage_event_id, "$.cache_read_tokens").value_or(0));
    request.cache_creation_5m_tokens =
        static_cast<int>(json_extract_int(conn, usage_event_id, "$.cache_creation_5m_tokens").value_or(0));
    request.cache_creation_1h_tokens =
        static_cast<int>(json_extract_int(conn, usage_event_id, "$.cache_creation_1h_tokens").value_or(0));
    request.output_tokens = static_cast<int>(json_extract_int(conn, usage_event_id, "$.output_tokens").value_or(0));
    request.tier_multiplier = json_extract_double(conn, usage_event_id, "$.tier_multiplier").value_or(1.0);
    request.channel_multiplier = json_extract_double(conn, usage_event_id, "$.channel_multiplier").value_or(1.0);
    request.endpoint = json_extract_string(conn, usage_event_id, "$.endpoint").value_or("");
    request.method = json_extract_string(conn, usage_event_id, "$.method").value_or("");
    request.status_code = static_cast<int>(json_extract_int(conn, usage_event_id, "$.status_code").value_or(0));
    request.latency_ms = static_cast<int>(json_extract_int(conn, usage_event_id, "$.latency_ms").value_or(0));
    request.first_token_latency_ms =
        static_cast<int>(json_extract_int(conn, usage_event_id, "$.first_token_latency_ms").value_or(0));
    request.error_class = json_extract_string(conn, usage_event_id, "$.error_class").value_or("");
    request.error_message = json_extract_string(conn, usage_event_id, "$.error_message").value_or("");
    request.channel_id = json_extract_int(conn, usage_event_id, "$.channel_id").value_or(0);
    request.is_stream = json_extract_bool(conn, usage_event_id, "$.is_stream").value_or(false);
    request.statue = json_extract_bool(conn, usage_event_id, "$.statue")
                         .value_or(json_extract_string(conn, usage_event_id, "$.status").value_or("committed") ==
                                   "committed");
    return request;
}

bool load_balance_debited(MysqlConnection &conn, long long usage_event_id)
{
    return json_extract_bool(conn, usage_event_id, "$.balance_debited").value_or(false);
}

bool load_retryable(MysqlConnection &conn, long long usage_event_id)
{
    return json_extract_bool(conn, usage_event_id, "$.retryable").value_or(false);
}

std::optional<UsageCommitJob> row_to_usage_commit_job(const MysqlResultRow &row)
{
    if (row.size() < 9 || !row[0].has_value()) {
        return std::nullopt;
    }
    UsageCommitJob job;
    job.id = std::stoll(*row[0]);
    job.usage_event_id = std::stoll(row[1].value_or("0"));
    job.user_id = std::stoll(row[2].value_or("0"));
    job.token_id = std::stoll(row[3].value_or("0"));
    job.state = row[4].value_or("");
    job.lease_token = row[5];
    job.lease_until = row[6];
    job.attempts = std::stoi(row[7].value_or("0"));
    job.created_at = row[8].value_or("");
    job.updated_at = row.size() > 9 ? row[9].value_or("") : "";
    return job;
}

std::string select_job_sql(std::string_view suffix)
{
    return std::string{
        "SELECT id,usage_event_id,user_id,token_id,state,lease_token,lease_until,attempts,created_at,updated_at "
        "FROM usage_commit_jobs "
    } + std::string{ suffix };
}

std::string valid_state(std::string_view state)
{
    const std::string normalized = trim_ascii(state);
    static const std::unordered_set<std::string> allowed = {
        std::string{ usage_commit_job_state_streaming },  std::string{ usage_commit_job_state_ready },
        std::string{ usage_commit_job_state_processing }, std::string{ usage_commit_job_state_done },
        std::string{ usage_commit_job_state_aborted },    std::string{ usage_commit_job_state_dead_letter },
    };
    if (!allowed.contains(normalized)) {
        assert(false && "internal: invalid usage commit job state");
        return {};
    }
    return normalized;
}

std::string value_or_now(const std::optional<std::string> &value)
{
    return value.has_value() && !trim_ascii(*value).empty() ? trim_ascii(*value) : usage_commit_timestamp_now();
}

} // namespace

std::string usage_commit_timestamp_now()
{
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string{ buffer };
}

UsageCommitJobStore::UsageCommitJobStore(MysqlConnection &conn)
    : conn_(conn)
{
}

long long UsageCommitJobStore::create_usage_commit_job(const UsageCommitJobInput &input)
{
    const UsageCommitJobInput normalized = normalize_job_input(input);
    const long long usage_event_id = normalized.usage_event_id;
    if (usage_event_id <= 0) {
        return 0;
    }

    conn_.exec("INSERT INTO usage_commit_jobs("
               "usage_event_id,user_id,token_id,state,attempts,payload_json,created_at,updated_at"
               ") VALUES(" +
               std::to_string(usage_event_id) + "," + std::to_string(normalized.user_id) + "," +
               std::to_string(normalized.token_id) + "," +
               conn_.quote(std::string{ usage_commit_job_state_streaming }) + ",0," +
               conn_.quote(request_to_payload_json(normalized.request, normalized.direct_commit,
                                                   normalized.balance_debited, normalized.retryable)) +
               ",CURRENT_TIMESTAMP,CURRENT_TIMESTAMP) ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id)");
    return static_cast<long long>(conn_.last_insert_id());
}

std::vector<long long>
UsageCommitJobStore::create_usage_commit_jobs_fast(const std::vector<UsageCommitJobInput> &inputs)
{
    std::unordered_map<long long, UsageCommitJobInput> deduped;
    std::vector<long long> order;
    for (const UsageCommitJobInput &input : inputs) {
        const long long usage_event_id = input.usage_event_id > 0 ? input.usage_event_id : input.request.id;
        if (usage_event_id <= 0) {
            continue;
        }
        if (!deduped.contains(usage_event_id)) {
            UsageCommitJobInput normalized = input;
            normalized.usage_event_id = usage_event_id;
            deduped.emplace(usage_event_id, normalize_job_input(normalized));
            order.push_back(usage_event_id);
        }
    }
    if (order.empty()) {
        return {};
    }

    std::string sql = "INSERT INTO usage_commit_jobs(usage_event_id,user_id,token_id,state,attempts,payload_json,"
                      "created_at,updated_at) VALUES";
    bool first = true;
    for (const long long usage_event_id : order) {
        const UsageCommitJobInput &input = deduped.at(usage_event_id);
        if (!first) {
            sql += ",";
        }
        first = false;
        sql += "(" + std::to_string(usage_event_id) + "," + std::to_string(input.user_id) + "," +
               std::to_string(input.token_id) + "," + conn_.quote(std::string{ usage_commit_job_state_streaming }) +
               ",0," +
               conn_.quote(request_to_payload_json(input.request, input.direct_commit, input.balance_debited,
                                                   input.retryable)) +
               ",CURRENT_TIMESTAMP,CURRENT_TIMESTAMP)";
    }
    sql += " ON DUPLICATE KEY UPDATE usage_event_id=usage_event_id";
    conn_.exec(sql);

    std::vector<long long> ids;
    ids.reserve(order.size());
    for (const long long usage_event_id : order) {
        const auto job = get_usage_commit_job_by_usage_event_id(usage_event_id);
        ids.push_back(job.has_value() ? job->id : 0);
    }
    return ids;
}

bool UsageCommitJobStore::finalize_usage_commit_job(const UsageCommitFinalizeInput &input)
{
    const std::string to_state = valid_state(input.to_state);
    const std::string from_state = input.from_state.empty() ? std::string{ usage_commit_job_state_streaming } :
                                                              valid_state(input.from_state);
    Request request = input.request;
    if (request.id <= 0 && input.job_id <= 0) {
        throw std::invalid_argument("job_id or request.id must be provided");
    }
    const std::string finished_at = value_or_now(input.finished_at);

    if (input.job_id > 0) {
        const long long usage_event_id =
            std::stoll(conn_
                           .query_one("SELECT usage_event_id FROM usage_commit_jobs WHERE id=" +
                                      std::to_string(input.job_id) + " LIMIT 1")
                           .value_or("0"));
        if (usage_event_id <= 0) {
            return false;
        }
        if (request.id <= 0) {
            request.id = usage_event_id;
        }
        if (request.user_id <= 0) {
            request.user_id = std::stoll(conn_
                                             .query_one("SELECT user_id FROM usage_commit_jobs WHERE id=" +
                                                        std::to_string(input.job_id) + " LIMIT 1")
                                             .value_or("0"));
        }
        if (request.token_id <= 0) {
            request.token_id = std::stoll(conn_
                                              .query_one("SELECT token_id FROM usage_commit_jobs WHERE id=" +
                                                         std::to_string(input.job_id) + " LIMIT 1")
                                              .value_or("0"));
        }
        conn_.exec("UPDATE usage_commit_jobs SET state=" + conn_.quote(to_state) +
                   ", payload_json=" +
                   conn_.quote(request_to_payload_json(request, false, input.balance_debited, input.retryable)) +
                   ", lease_token=NULL, lease_until=NULL, updated_at=" + conn_.quote(finished_at) +
                   " WHERE id=" + std::to_string(input.job_id) + " AND state=" + conn_.quote(from_state));
        return conn_.affected_rows() > 0;
    }

    assert(request.user_id > 0 && "internal: request.user_id must be positive");
    if (request.user_id <= 0) {
        return false;
    }
    assert(request.token_id > 0 && "internal: request.token_id must be positive");
    if (request.token_id <= 0) {
        return false;
    }
    assert(request.id > 0 && "internal: request.id must be positive");
    if (request.id <= 0) {
        return false;
    }
    conn_.exec("INSERT INTO usage_commit_jobs("
               "usage_event_id,user_id,token_id,state,attempts,payload_json,created_at,updated_at"
               ") VALUES(" +
               std::to_string(request.id) + "," + std::to_string(request.user_id) + "," +
               std::to_string(request.token_id) + "," + conn_.quote(to_state) + ",0," +
               conn_.quote(request_to_payload_json(request, false, input.balance_debited, input.retryable)) + "," +
               conn_.quote(finished_at) + "," + conn_.quote(finished_at) +
               ") ON DUPLICATE KEY UPDATE "
               "state=IF(state IN(" +
               conn_.quote(from_state) + "," + conn_.quote(to_state) +
               "),VALUES(state),state),"
               "payload_json=IF(state IN(" +
               conn_.quote(from_state) + "," + conn_.quote(to_state) +
               "),VALUES(payload_json),payload_json),"
               "lease_token=IF(state IN(" +
               conn_.quote(from_state) + "," + conn_.quote(to_state) +
               "),NULL,lease_token),"
               "lease_until=IF(state IN(" +
               conn_.quote(from_state) + "," + conn_.quote(to_state) +
               "),NULL,lease_until),"
               "updated_at=IF(state IN(" +
               conn_.quote(from_state) + "," + conn_.quote(to_state) + "),VALUES(updated_at),updated_at)");
    return true;
}

std::vector<UsageCommitJob> UsageCommitJobStore::claim_ready_usage_commit_jobs(const UsageCommitClaimInput &input)
{
    const int limit = input.limit > 0 ? input.limit : 64;
    const std::string lease_token = trim_ascii(input.lease_token);
    if (lease_token.empty()) {
        throw std::invalid_argument("lease_token must not be empty");
    }
    const std::string lease_until = input.lease_until.has_value() ? trim_ascii(*input.lease_until) :
                                                                    usage_commit_timestamp_now();

    DbTransaction tr(conn_);
    std::vector<MysqlResultRow> selected_rows;
    selected_rows.reserve(static_cast<size_t>(limit));

    const auto append_rows = [&](std::vector<MysqlResultRow> rows) {
        const size_t remaining =
            static_cast<size_t>(limit) > selected_rows.size() ? static_cast<size_t>(limit) - selected_rows.size() : 0U;
        for (MysqlResultRow &row : rows) {
            if (remaining == 0 || selected_rows.size() >= static_cast<size_t>(limit)) {
                break;
            }
            selected_rows.push_back(std::move(row));
        }
    };

    append_rows(conn_.query_rows(
        "SELECT id,usage_event_id,user_id,token_id,state,lease_token,lease_until,attempts,created_at,updated_at "
        "FROM usage_commit_jobs WHERE state=" +
        conn_.quote(std::string{ usage_commit_job_state_processing }) +
        " AND lease_until IS NOT NULL AND lease_until < CURRENT_TIMESTAMP "
        "ORDER BY lease_until ASC,id ASC LIMIT " +
        std::to_string(limit) + " FOR UPDATE SKIP LOCKED"));
    if (selected_rows.size() < static_cast<size_t>(limit)) {
        append_rows(conn_.query_rows(
            "SELECT id,usage_event_id,user_id,token_id,state,lease_token,lease_until,attempts,created_at,updated_at "
            "FROM usage_commit_jobs WHERE state=" +
            conn_.quote(std::string{ usage_commit_job_state_ready }) + " ORDER BY updated_at ASC,id ASC LIMIT " +
            std::to_string(limit - static_cast<int>(selected_rows.size())) + " FOR UPDATE SKIP LOCKED"));
    }

    std::vector<UsageCommitJob> jobs;
    jobs.reserve(selected_rows.size());
    std::vector<std::string> ids;
    ids.reserve(selected_rows.size());
    for (const MysqlResultRow &row : selected_rows) {
        auto job = row_to_usage_commit_job(row);
        if (!job.has_value()) {
            continue;
        }
        ids.push_back(std::to_string(job->id));
        jobs.push_back(std::move(*job));
    }

    if (!ids.empty()) {
        std::string id_list;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i > 0) {
                id_list += ",";
            }
            id_list += ids[i];
        }
        conn_.exec(
            "UPDATE usage_commit_jobs SET state=" + conn_.quote(std::string{ usage_commit_job_state_processing }) +
            ", lease_token=" + conn_.quote(lease_token) + ", lease_until=" + conn_.quote(lease_until) +
            ", attempts=attempts+1, updated_at=CURRENT_TIMESTAMP WHERE id IN (" + id_list + ")");
        for (UsageCommitJob &job : jobs) {
            job.state = std::string{ usage_commit_job_state_processing };
            job.lease_token = lease_token;
            job.lease_until = lease_until;
            job.attempts += 1;
            job.request = load_request_by_usage_event_id(conn_, job.usage_event_id);
        }
    }

    tr.commit();
    return jobs;
}

bool UsageCommitJobStore::extend_processing_usage_commit_jobs_lease(std::string_view lease_token,
                                                                    std::string_view lease_until)
{
    const std::string normalized_lease = trim_ascii(lease_token);
    if (normalized_lease.empty()) {
        throw std::invalid_argument("lease_token must not be empty");
    }
    conn_.exec(
        "UPDATE usage_commit_jobs SET lease_until=" + conn_.quote(trim_ascii(lease_until)) +
        ", updated_at=CURRENT_TIMESTAMP WHERE state=" + conn_.quote(std::string{ usage_commit_job_state_processing }) +
        " AND lease_token=" + conn_.quote(normalized_lease));
    return conn_.affected_rows() > 0;
}

bool UsageCommitJobStore::update_processing_usage_commit_job_state(long long job_id, std::string_view lease_token,
                                                                   std::string_view to_state)
{
    assert(job_id > 0 && "internal: job_id must be positive");
    if (job_id <= 0) {
        return false;
    }
    const std::string normalized_lease = trim_ascii(lease_token);
    if (normalized_lease.empty()) {
        throw std::invalid_argument("lease_token must not be empty");
    }
    const std::string normalized_state = valid_state(to_state);
    conn_.exec("UPDATE usage_commit_jobs SET state=" + conn_.quote(normalized_state) +
               ", lease_token=NULL, lease_until=NULL, updated_at=CURRENT_TIMESTAMP WHERE id=" + std::to_string(job_id) +
               " AND state=" + conn_.quote(std::string{ usage_commit_job_state_processing }) +
               " AND lease_token=" + conn_.quote(normalized_lease));
    return conn_.affected_rows() > 0;
}

long long UsageCommitJobStore::abort_stale_streaming_usage_commit_jobs(std::string_view before_time)
{
    conn_.exec("UPDATE usage_commit_jobs SET state=" + conn_.quote(std::string{ usage_commit_job_state_aborted }) +
               ", lease_token=NULL, lease_until=NULL, updated_at=CURRENT_TIMESTAMP WHERE state=" +
               conn_.quote(std::string{ usage_commit_job_state_streaming }) + " AND updated_at < " +
               conn_.quote(trim_ascii(before_time)));
    return static_cast<long long>(conn_.affected_rows());
}

bool UsageCommitJobStore::complete_usage_commit_job(long long job_id, std::string_view lease_token,
                                                    std::string_view finished_at)
{
    UsageCommitCompletionResult stats;
    return commit_claimed_job(job_id, lease_token, finished_at, &stats);
}

UsageCommitCompletionResult UsageCommitJobStore::complete_usage_commit_jobs(const std::vector<UsageCommitJob> &jobs,
                                                                            std::string_view lease_token,
                                                                            std::string_view finished_at)
{
    UsageCommitCompletionResult stats;
    for (const UsageCommitJob &job : jobs) {
        (void)commit_claimed_job(job.id, lease_token, finished_at, &stats);
    }
    return stats;
}

bool UsageCommitJobStore::commit_usage_payload_direct(const UsageCommitJobInput &input, std::string_view finished_at)
{
    try {
        const UsageCommitJobInput normalized = normalize_job_input(input);
        return write_usage_event(normalized.request, normalized.balance_debited, finished_at);
    } catch (const std::exception &) {
        return false;
    }
}

bool UsageCommitJobStore::commit_claimed_job(long long job_id, std::string_view lease_token,
                                             std::string_view finished_at, UsageCommitCompletionResult *stats)
{
    assert(job_id > 0 && "internal: job_id must be positive");
    if (job_id <= 0) {
        return false;
    }
    const std::string normalized_lease = trim_ascii(lease_token);
    if (normalized_lease.empty()) {
        throw std::invalid_argument("lease_token must not be empty");
    }

    const std::string done_at = trim_ascii(finished_at).empty() ? usage_commit_timestamp_now() :
                                                                  trim_ascii(finished_at);
    DbTransaction tr(conn_);
    const auto rows = conn_.query_rows(
        "SELECT id,usage_event_id,user_id,token_id,state,lease_token,lease_until,attempts,created_at,updated_at,"
        "(CASE WHEN lease_until IS NOT NULL AND lease_until >= CURRENT_TIMESTAMP THEN 1 ELSE 0 END) "
        "FROM usage_commit_jobs WHERE id=" +
        std::to_string(job_id) + " LIMIT 1 FOR UPDATE");
    if (rows.empty()) {
        return false;
    }

    UsageCommitJob job = row_to_usage_commit_job(rows.front()).value();
    job.request = load_request_by_usage_event_id(conn_, job.usage_event_id);
    const bool balance_debited = load_balance_debited(conn_, job.usage_event_id);
    const bool retryable = load_retryable(conn_, job.usage_event_id);
    const bool lease_valid = rows.front().size() > 10 && rows.front()[10].value_or("0") == "1";
    if (job.state == usage_commit_job_state_done || job.state == usage_commit_job_state_aborted) {
        tr.commit();
        return true;
    }
    if (job.state != usage_commit_job_state_processing || job.lease_token.value_or("") != normalized_lease ||
        !lease_valid) {
        return false;
    }

    bool wrote = false;
    try {
        wrote = write_usage_event(job.request, balance_debited, done_at);
    } catch (const std::exception &) {
        wrote = false;
    }

    std::string next_state = std::string{ usage_commit_job_state_done };
    if (!wrote) {
        next_state = retryable ? std::string{ usage_commit_job_state_ready } :
                                 std::string{ usage_commit_job_state_dead_letter };
    }
    conn_.exec("UPDATE usage_commit_jobs SET state=" + conn_.quote(next_state) +
               ", lease_token=NULL, lease_until=NULL, updated_at=" + conn_.quote(done_at) + " WHERE id=" +
               std::to_string(job_id) + " AND state=" + conn_.quote(std::string{ usage_commit_job_state_processing }) +
               " AND lease_token=" + conn_.quote(normalized_lease));
    if (conn_.affected_rows() == 0) {
        return false;
    }
    tr.commit();
    if (stats != nullptr) {
        if (next_state == usage_commit_job_state_done) {
            stats->completed += 1;
        } else if (next_state == usage_commit_job_state_dead_letter) {
            stats->dead_lettered += 1;
        } else if (next_state == usage_commit_job_state_ready) {
            stats->requeued += 1;
        }
    }
    return true;
}

bool UsageCommitJobStore::write_usage_event(const Request &request, bool balance_debited, std::string_view finished_at)
{
    if (request.id <= 0 || request.user_id <= 0 || request.token_id <= 0) {
        return false;
    }
    if (count_usage_events_by_id(request.id) > 0) {
        return true;
    }

    const std::string occurred_at = !trim_ascii(request.time).empty() ? trim_ascii(request.time) :
                                                                        trim_ascii(finished_at);
    const std::string status = "committed";
    const std::optional<std::string> model_name =
        request.model.name.empty() ? std::nullopt : std::optional<std::string>{ request.model.name };
    const std::optional<std::string> service_tier =
        request.service_tier.empty() ? std::nullopt : std::optional<std::string>{ request.service_tier };
    const std::optional<std::string> endpoint =
        request.endpoint.empty() ? std::nullopt : std::optional<std::string>{ request.endpoint };
    const std::optional<std::string> method =
        request.method.empty() ? std::nullopt : std::optional<std::string>{ request.method };
    const std::optional<std::string> error_class =
        request.error_class.empty() ? std::nullopt : std::optional<std::string>{ request.error_class };
    const std::optional<std::string> error_message =
        request.error_message.empty() ? std::nullopt : std::optional<std::string>{ request.error_message };

    DbTransaction tr(conn_);
    conn_.exec("INSERT INTO usage_events("
               "id,time,endpoint,method,status_code,latency_ms,first_token_latency_ms,"
               "error_class,error_message,user_id,token_id,channel_id,status,model,service_tier,"
               "input_tokens,cache_read_tokens,cache_creation_5m_tokens,cache_creation_1h_tokens,"
               "output_tokens,tier_multiplier,channel_multiplier,is_stream"
               ") VALUES(" +
               std::to_string(request.id) + "," + conn_.quote(occurred_at) + "," + sql_nullable(conn_, endpoint) +
               "," + sql_nullable(conn_, method) + "," + std::to_string(std::max(request.status_code, 0)) + "," +
               std::to_string(std::max(request.latency_ms, 0)) + "," +
               std::to_string(std::max(request.first_token_latency_ms, 0)) + "," +
               sql_nullable(conn_, error_class) + "," + sql_nullable(conn_, error_message) + "," +
               std::to_string(request.user_id) + "," + std::to_string(request.token_id) + "," +
               std::to_string(request.channel_id) + "," + conn_.quote(status) + "," +
               sql_nullable(conn_, model_name) + "," + sql_nullable(conn_, service_tier) + "," +
               std::to_string(std::max(request.input_tokens, 0)) + "," +
               std::to_string(std::max(request.cache_read_tokens, 0)) + "," +
               std::to_string(std::max(request.cache_creation_5m_tokens, 0)) + "," +
               std::to_string(std::max(request.cache_creation_1h_tokens, 0)) + "," +
               std::to_string(std::max(request.output_tokens, 0)) + "," + json_double(request.tier_multiplier) + "," +
               json_double(request.channel_multiplier) + "," + std::to_string(request.is_stream ? 1 : 0) + ")");
    if (conn_.affected_rows() == 0) {
        return false;
    }
    if (!debit_request_balance(conn_, request, balance_debited)) {
        return false;
    }
    tr.commit();
    return true;
}

std::optional<UsageCommitJob> UsageCommitJobStore::get_usage_commit_job_by_id(long long job_id)
{
    assert(job_id > 0 && "internal: job_id must be positive");
    if (job_id <= 0) {
        return std::nullopt;
    }
    const auto rows = conn_.query_rows(select_job_sql("WHERE id=" + std::to_string(job_id) + " LIMIT 1"));
    if (rows.empty()) {
        return std::nullopt;
    }
    auto job = row_to_usage_commit_job(rows.front());
    if (!job.has_value()) {
        return std::nullopt;
    }
    job->request = load_request_by_usage_event_id(conn_, job->usage_event_id);
    return job;
}

std::optional<UsageCommitJob> UsageCommitJobStore::get_usage_commit_job_by_usage_event_id(long long usage_event_id)
{
    assert(usage_event_id > 0 && "internal: usage_event_id must be positive");
    if (usage_event_id <= 0) {
        return std::nullopt;
    }
    const auto rows =
        conn_.query_rows(select_job_sql("WHERE usage_event_id=" + std::to_string(usage_event_id) + " LIMIT 1"));
    if (rows.empty()) {
        return std::nullopt;
    }
    auto job = row_to_usage_commit_job(rows.front());
    if (!job.has_value()) {
        return std::nullopt;
    }
    job->request = load_request_by_usage_event_id(conn_, job->usage_event_id);
    return job;
}

long long UsageCommitJobStore::count_usage_events_by_id(long long usage_event_id)
{
    return std::stoll(
        conn_.query_one("SELECT COUNT(*) FROM usage_events WHERE id=" + std::to_string(usage_event_id)).value_or("0"));
}

std::optional<std::string> UsageCommitJobStore::usage_event_status_by_id(long long usage_event_id)
{
    return conn_.query_one("SELECT status FROM usage_events WHERE id=" + std::to_string(usage_event_id) + " LIMIT 1");
}

UsageCommitWorker::UsageCommitWorker(UsageCommitJobStore &store, const Config &config)
    : store_(store)
{
    claim_size_ = config.usage_commit_claim_size > 0 ? config.usage_commit_claim_size : 64;
    worker_count_ = config.usage_commit_workers > 0 ? config.usage_commit_workers : 1;
    lease_ms_ = config.usage_commit_lease_ms > 0 ? config.usage_commit_lease_ms : 15000;
    metrics_.worker_count = worker_count_;
    metrics_.claim_size = claim_size_;
}

UsageCommitWorkerMetrics UsageCommitWorker::drain_once()
{
    const std::string lease_token = make_lease_token();
    const auto now = std::chrono::system_clock::now() + std::chrono::milliseconds(lease_ms_);
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    const auto jobs =
        store_.claim_ready_usage_commit_jobs(UsageCommitClaimInput{ claim_size_, lease_token, std::string{ buffer } });
    metrics_.claimed_total += jobs.size();
    if (!jobs.empty()) {
        const auto result = store_.complete_usage_commit_jobs(jobs, lease_token, usage_commit_timestamp_now());
        metrics_.completed_total += result.completed;
        metrics_.dead_letter_total += result.dead_lettered;
        metrics_.requeued_total += result.requeued;
    }
    return metrics_;
}

std::string UsageCommitWorker::make_lease_token() const
{
    std::mt19937_64 rng{ std::random_device{}() };
    std::ostringstream out;
    out << std::hex << rng() << rng();
    return out.str();
}

AsyncStreamUsageSink::AsyncStreamUsageSink(UsageCommitJobStore &store, const Config &config)
    : store_(store)
{
    batch_size_ = config.usage_finalize_batch_size > 0 ? static_cast<size_t>(config.usage_finalize_batch_size) : 256U;
    queue_size_ = config.usage_finalize_queue_size > 0 ? static_cast<size_t>(config.usage_finalize_queue_size) : 4096U;
}

bool AsyncStreamUsageSink::enqueue_or_commit_direct(const UsageCommitJobInput &input)
{
    if (queued_.size() >= queue_size_) {
        fallback_sync_total_ += 1;
        return store_.commit_usage_payload_direct(input, usage_commit_timestamp_now());
    }
    queued_.push_back(input);
    return true;
}

std::vector<long long> AsyncStreamUsageSink::flush()
{
    if (queued_.empty()) {
        return {};
    }
    std::vector<UsageCommitJobInput> batch;
    batch.swap(queued_);
    if (batch.size() > batch_size_) {
        queued_.insert(queued_.end(), batch.begin() + static_cast<long long>(batch_size_), batch.end());
        batch.resize(batch_size_);
    }
    return store_.create_usage_commit_jobs_fast(batch);
}

unsigned long long AsyncStreamUsageSink::fallback_sync_total() const
{
    return fallback_sync_total_;
}

size_t AsyncStreamUsageSink::queue_depth() const
{
    return queued_.size();
}

void run_usage_commit_runtime_tick(UsageCommitJobStore &store, AsyncStreamUsageSink &sink, UsageCommitWorker &worker,
                                   std::string_view stale_before_time)
{
    (void)sink.flush();
    (void)store.abort_stale_streaming_usage_commit_jobs(stale_before_time);
    (void)worker.drain_once();
}

} // namespace revlm
