#include "request/request.hpp"

#include "users/users.hpp"
#include "store/database.hpp"
#include "revlm_entities-odb.hxx"
#include "util/strings.hpp"

#include <odb/database.hxx>
#include <odb/mysql/query.hxx>
#include <odb/query.hxx>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revlm
{

bool Request::commit(std::string_view finished_at)
{
    odb::database &db = database();
    if (id <= 0 || user_id <= 0 || token_id <= 0) {
        return false;
    }

    ScopedTransaction t(db);
    using query = odb::query<Request>;
    if (!db.query<Request>(query::id == id).empty()) {
        t.commit();
        return true;
    }

    const std::string occurred_at = !trim_ascii(time).empty()        ? trim_ascii(time) :
                                    !trim_ascii(finished_at).empty() ? trim_ascii(finished_at) :
                                                                       request_timestamp_now();
    time = occurred_at;
    if (date.empty() && occurred_at.size() >= 10) {
        date = occurred_at.substr(0, 10);
    }
    if ((model_name.null() || model_name->empty()) && pricing_model != nullptr && !pricing_model->name.empty()) {
        model_name = pricing_model->name;
    }

    db.persist(*this);
    hydrate_request_model(*this);

    UserStore::instance().tokens().requests().apply_total(*this);
    t.commit();
    return true;
}

RequestStore::RequestStore()
    : db_(database())
{
}

std::vector<Request> RequestStore::query(const RequestListFilter &filter)
{
    using query = odb::query<Request>;
    query pred(true);
    if (filter.id.has_value()) {
        pred = pred && query::id == *filter.id;
    }
    if (filter.user_id.has_value()) {
        pred = pred && query::user_id == *filter.user_id;
    }
    if (filter.token_id.has_value()) {
        pred = pred && query::token_id == *filter.token_id;
    }
    if (filter.channel_id.has_value()) {
        pred = pred && query::channel_id == *filter.channel_id;
    }
    if (filter.start.has_value()) {
        pred = pred && query::time >= *filter.start;
    }
    if (filter.end_exclusive.has_value()) {
        pred = pred && query::time < *filter.end_exclusive;
    }
    if (filter.model_exact.has_value()) {
        pred = pred && query::model_name == *filter.model_exact;
    }
    if (filter.model_like.has_value() && !filter.model_like->empty()) {
        pred = pred && query::model_name.like("%" + *filter.model_like + "%");
    }
    if (filter.before_id.has_value()) {
        pred = pred && query::id < *filter.before_id;
    }
    if (filter.after_id.has_value()) {
        pred = pred && query::id > *filter.after_id;
    }
    if (!filter.user_ids.empty()) {
        pred = pred && query::user_id.in_range(filter.user_ids.begin(), filter.user_ids.end());
    }
    if (!filter.channel_ids.empty()) {
        pred = pred && query::channel_id.in_range(filter.channel_ids.begin(), filter.channel_ids.end());
    }

    query order = filter.order_asc ? ("ORDER BY" + query::id) : ("ORDER BY" + query::id + "DESC");
    if (filter.limit > 0) {
        order = order + "LIMIT" + query::_val(filter.limit);
    }

    ScopedTransaction t(db_);
    std::vector<Request> out;
    for (Request &req : db_.query<Request>(pred + order)) {
        if (req.date.empty() && req.time.size() >= 10) {
            req.date = req.time.substr(0, 10);
        }
        hydrate_request_model(req);
        out.push_back(std::move(req));
    }
    t.commit();
    return out;
}

std::vector<Request> RequestStore::list(long long user_id, long long token_id, std::string start, std::string end,
                                        std::string model, int limit)
{
    if (user_id <= 0 || token_id <= 0) {
        return {};
    }
    if (limit <= 0) {
        limit = 50;
    }
    if (limit > 200) {
        limit = 200;
    }

    RequestListFilter filter;
    filter.user_id = user_id;
    filter.token_id = token_id;
    filter.start = std::move(start);
    filter.end_exclusive = std::move(end);
    if (!model.empty()) {
        filter.model_exact = std::move(model);
    }
    filter.limit = limit;
    return query(filter);
}

std::optional<Request> RequestStore::get(long long user_id, long long token_id, long long id)
{
    if (user_id <= 0 || token_id <= 0 || id <= 0) {
        return std::nullopt;
    }
    RequestListFilter filter;
    filter.id = id;
    filter.user_id = user_id;
    filter.token_id = token_id;
    filter.limit = 1;
    auto rows = query(filter);
    if (rows.empty()) {
        return std::nullopt;
    }
    return std::move(rows.front());
}

std::optional<Request> RequestStore::get_by_id(long long id)
{
    if (id <= 0) {
        return std::nullopt;
    }
    RequestListFilter filter;
    filter.id = id;
    filter.limit = 1;
    auto rows = query(filter);
    if (rows.empty()) {
        return std::nullopt;
    }
    return std::move(rows.front());
}

std::vector<RequestTotal> RequestStore::totals(long long user_id, long long token_id, std::string start_date,
                                               std::string end_date)
{
    if (user_id <= 0 || token_id <= 0) {
        return {};
    }

    using query = odb::query<RequestTotal>;
    ScopedTransaction t(db_);
    std::vector<RequestTotal> out;
    for (const RequestTotal &total :
         db_.query<RequestTotal>(query::id.user_id == user_id && query::id.token_id == token_id &&
                                 query::id.date >= start_date && query::id.date <= end_date)) {
        out.push_back(total);
    }
    t.commit();
    return out;
}

void RequestStore::apply_total(const Request &request)
{
    if (request.user_id <= 0 || request.token_id <= 0 || request.date.empty()) {
        return;
    }
    const int cache_creation =
        std::max(request.cache_creation_5m_tokens, 0) + std::max(request.cache_creation_1h_tokens, 0);
    const int total_tokens = std::max(request.input_tokens, 0) + std::max(request.output_tokens, 0) +
                             std::max(request.cache_read_tokens, 0) + cache_creation;
    const double usd = request.solve_price();
    const int ftl = std::max(request.first_token_latency_ms, 0);

    ScopedTransaction t(db_);
    using query = odb::query<RequestTotal>;
    auto existing = db_.query<RequestTotal>(query::id.user_id == request.user_id &&
                                            query::id.token_id == request.token_id && query::id.date == request.date);
    auto it = existing.begin();
    if (it != existing.end()) {
        RequestTotal total = *it;
        total.requests += 1;
        total.input_tokens += std::max(request.input_tokens, 0);
        total.output_tokens += std::max(request.output_tokens, 0);
        total.cache_read_tokens += std::max(request.cache_read_tokens, 0);
        total.cache_creation_tokens += cache_creation;
        total.tokens += total_tokens;
        total.usd += usd;
        total.first_token_latency_sum += ftl;
        db_.update(total);
    } else {
        RequestTotal total;
        total.id.user_id = request.user_id;
        total.id.token_id = request.token_id;
        total.id.date = request.date;
        total.requests = 1;
        total.input_tokens = std::max(request.input_tokens, 0);
        total.output_tokens = std::max(request.output_tokens, 0);
        total.cache_read_tokens = std::max(request.cache_read_tokens, 0);
        total.cache_creation_tokens = cache_creation;
        total.tokens = total_tokens;
        total.usd = usd;
        total.first_token_latency_sum = ftl;
        db_.persist(total);
    }
    t.commit();
}

} // namespace revlm
