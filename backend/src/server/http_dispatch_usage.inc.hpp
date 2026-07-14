// Usage / request analytics handlers (merged from former usage/*_api.cpp).
// Kept in the http_dispatch translation unit's anonymous namespace.

constexpr std::string_view kAdminTimeZone = "Asia/Shanghai";

struct UsageQueryOptions {
    std::string time_zone = "UTC";
    bool all_time = false;
    std::optional<sys_seconds> start_utc;
    std::optional<sys_seconds> end_exclusive_utc;
    std::optional<long long> token_id;
};

std::optional<std::string> nullable_odb_string(const odb::nullable<std::string> &value)
{
    if (value.null() || value->empty()) {
        return std::nullopt;
    }
    return *value;
}

std::string mysql_to_iso_utc(std::string_view value)
{
    if (value.empty()) {
        return {};
    }
    std::string out{ value };
    if (out.size() >= 19 && out[10] == ' ') {
        out[10] = 'T';
    }
    if (!out.empty() && out.back() != 'Z') {
        out.push_back('Z');
    }
    return out;
}

std::string status_json_label(std::string_view status)
{
    if (status == "committed" || status == "1" || status == "true" || status == "TRUE") {
        return "committed";
    }
    return std::string{ status };
}

std::string model_icon_url(std::string_view owned_by)
{
    const std::string owner = lowercase_ascii(trim_ascii(owned_by));
    if (owner.empty()) {
        return {};
    }
    return "/assets/model-icons/" + owner +
           (owner == "openai" || owner == "xai" || owner == "openrouter" || owner == "ollama" ? ".svg" : "-color.svg");
}

bool parse_usage_query_options(const std::map<std::string, std::string> &params, UsageQueryOptions &out,
                               std::string &message)
{
    out = UsageQueryOptions{};
    out.time_zone = trim_ascii(query_param_value(params, "tz"));
    if (out.time_zone.empty()) {
        out.time_zone = "UTC";
    }
    if (!zone_exists(out.time_zone)) {
        message = "tz 无效";
        return false;
    }

    const std::string all_time_raw = trim_ascii(query_param_value(params, "all_time"));
    if (!all_time_raw.empty()) {
        bool all_time = false;
        if (!parse_bool_flag(all_time_raw, all_time)) {
            message = "all_time 无效";
            return false;
        }
        out.all_time = all_time;
    }

    const std::string start = trim_ascii(query_param_value(params, "start"));
    const std::string end = trim_ascii(query_param_value(params, "end"));
    if (!start.empty()) {
        int y = 0;
        int m = 0;
        int d = 0;
        if (!parse_date_yyyy_mm_dd(start, y, m, d)) {
            message = "start 无效";
            return false;
        }
        out.start_utc = local_date_to_utc(y, static_cast<unsigned>(m), static_cast<unsigned>(d), out.time_zone);
    }
    if (!end.empty()) {
        int y = 0;
        int m = 0;
        int d = 0;
        if (!parse_date_yyyy_mm_dd(end, y, m, d)) {
            message = "end 无效";
            return false;
        }
        unsigned um = static_cast<unsigned>(m);
        unsigned ud = static_cast<unsigned>(d);
        const sys_seconds end_start = local_date_to_utc(y, um, ud, out.time_zone);
        next_date(y, um, ud);
        out.end_exclusive_utc = local_date_to_utc(y, um, ud, out.time_zone);
        if (*out.end_exclusive_utc <= end_start) {
            out.end_exclusive_utc = end_start + std::chrono::seconds{ 86400 };
        }
    }
    if (out.start_utc.has_value() && out.end_exclusive_utc.has_value() && *out.start_utc >= *out.end_exclusive_utc) {
        message = "日期范围无效";
        return false;
    }

    const std::string token_id_raw = trim_ascii(query_param_value(params, "token_id"));
    if (!token_id_raw.empty()) {
        long long token_id = 0;
        if (!parse_i64(token_id_raw, token_id) || token_id <= 0) {
            message = "token_id 无效";
            return false;
        }
        out.token_id = token_id;
    }
    return true;
}

RequestListFilter filter_from_usage_options(long long user_id, const UsageQueryOptions &options,
                                            bool committed_only = false)
{
    RequestListFilter filter;
    filter.user_id = user_id;
    if (options.token_id.has_value()) {
        filter.token_id = options.token_id;
    }
    if (!options.all_time) {
        if (options.start_utc.has_value()) {
            filter.start = to_mysql_datetime(*options.start_utc);
        }
        if (options.end_exclusive_utc.has_value()) {
            filter.end_exclusive = to_mysql_datetime(*options.end_exclusive_utc);
        }
    }
    if (committed_only) {
        filter.status = "committed";
    }
    return filter;
}

boost::json::object pricing_breakdown_to_json(const PricingBreakdown &p)
{
    boost::json::object o;
    if (p.model_public_id.has_value()) {
        o["model_public_id"] = *p.model_public_id;
    } else {
        o["model_public_id"] = nullptr;
    }
    o["model_found"] = p.model_found;
    if (p.owned_by.has_value()) {
        o["owned_by"] = *p.owned_by;
    } else {
        o["owned_by"] = nullptr;
    }
    if (p.service_tier.has_value()) {
        o["service_tier"] = *p.service_tier;
    } else {
        o["service_tier"] = nullptr;
    }
    o["pricing_kind"] = p.pricing_kind;
    o["input_tokens_total"] = p.input_tokens_total;
    o["input_tokens_cache_read"] = p.input_tokens_cache_read;
    o["input_tokens_cache_creation"] = p.input_tokens_cache_creation;
    o["input_tokens_cache_creation_5m"] = p.input_tokens_cache_creation_5m;
    o["input_tokens_cache_creation_1h"] = p.input_tokens_cache_creation_1h;
    o["input_tokens_billable"] = p.input_tokens_billable;
    o["output_tokens_total"] = p.output_tokens_total;
    o["input_usd_per_1m"] = p.input_usd_per_1m;
    o["output_usd_per_1m"] = p.output_usd_per_1m;
    o["cache_read_usd_per_1m"] = p.cache_read_usd_per_1m;
    o["cache_creation_5m_usd_per_1m"] = p.cache_creation_5m_usd_per_1m;
    o["cache_creation_1h_usd_per_1m"] = p.cache_creation_1h_usd_per_1m;
    o["input_cost_usd"] = p.input_cost_usd;
    o["output_cost_usd"] = p.output_cost_usd;
    o["cache_read_cost_usd"] = p.cache_read_cost_usd;
    o["cache_creation_cost_usd"] = p.cache_creation_cost_usd;
    o["cache_creation_5m_cost_usd"] = p.cache_creation_5m_cost_usd;
    o["cache_creation_1h_cost_usd"] = p.cache_creation_1h_cost_usd;
    o["base_cost_usd"] = p.base_cost_usd;
    o["tier_multiplier"] = p.tier_multiplier;
    o["channel_multiplier"] = p.channel_multiplier;
    o["final_cost_usd"] = p.final_cost_usd;
    return o;
}

boost::json::object request_to_user_event_json(const Request &req)
{
    boost::json::object o;
    o["id"] = req.id;
    o["time"] = mysql_to_iso_utc(req.time);
    o["request_id"] = std::to_string(req.id);
    const auto endpoint = nullable_odb_string(req.endpoint);
    const auto method = nullable_odb_string(req.method);
    const auto service_tier = nullable_odb_string(req.service_tier);
    const auto error_class = nullable_odb_string(req.error_class);
    const auto error_message = nullable_odb_string(req.error_message);
    if (endpoint.has_value()) {
        o["endpoint"] = *endpoint;
    } else {
        o["endpoint"] = nullptr;
    }
    if (method.has_value()) {
        o["method"] = *method;
    } else {
        o["method"] = nullptr;
    }
    o["token_id"] = req.token_id;
    o["channel_id"] = req.channel_id > 0 ? boost::json::value(req.channel_id) : boost::json::value(nullptr);
    o["status"] = status_json_label(req.status);
    if (!req.model.name.empty()) {
        o["model"] = req.model.name;
    } else {
        o["model"] = nullptr;
    }
    if (service_tier.has_value()) {
        o["service_tier"] = *service_tier;
    } else {
        o["service_tier"] = nullptr;
    }
    o["input_tokens"] = req.input_tokens;
    o["cache_read_tokens"] = req.cache_read_tokens;
    o["cache_creation_5m_tokens"] = req.cache_creation_5m_tokens;
    o["cache_creation_1h_tokens"] = req.cache_creation_1h_tokens;
    o["cache_creation_tokens"] = req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
    o["output_tokens"] = req.output_tokens;
    o["committed_usd"] = request_detail::decimal_to_string(req.solve_price());
    o["status_code"] = req.status_code;
    o["latency_ms"] = req.latency_ms;
    if (error_class.has_value()) {
        o["error_class"] = *error_class;
    } else {
        o["error_class"] = nullptr;
    }
    if (error_message.has_value()) {
        o["error_message"] = *error_message;
    } else {
        o["error_message"] = nullptr;
    }
    o["is_stream"] = req.is_stream;
    return o;
}

boost::json::object aggregate_window(const std::vector<Request> &rows, const UsageQueryOptions &options,
                                     double balance_usd)
{
    long long requests = 0;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_tokens = 0;
    long long cache_creation_tokens = 0;
    long long first_token_sum = 0;
    long long first_token_samples = 0;
    long long decode_tokens = 0;
    long long decode_latency_ms = 0;
    double committed = 0.0;
    std::optional<sys_seconds> min_time;
    std::optional<sys_seconds> max_time;

    for (const Request &req : rows) {
        if (req.status != "committed") {
            continue;
        }
        ++requests;
        input_tokens += req.input_tokens;
        output_tokens += req.output_tokens;
        cache_read_tokens += req.cache_read_tokens;
        cache_creation_tokens += req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
        committed += req.solve_price();
        if (req.first_token_latency_ms > 0) {
            first_token_sum += req.first_token_latency_ms;
            ++first_token_samples;
        }
        if (req.latency_ms > req.first_token_latency_ms && req.output_tokens > 0) {
            decode_tokens += req.output_tokens;
            decode_latency_ms += req.latency_ms - req.first_token_latency_ms;
        }
        if (!req.time.empty()) {
            try {
                const sys_seconds tp = parse_mysql_datetime(req.time);
                if (!min_time.has_value() || tp < *min_time) {
                    min_time = tp;
                }
                if (!max_time.has_value() || tp > *max_time) {
                    max_time = tp;
                }
            } catch (const std::exception &) {
            }
        }
    }

    const long long tokens = input_tokens + output_tokens + cache_read_tokens + cache_creation_tokens;
    std::string since;
    std::string until;
    if (!options.all_time) {
        if (options.start_utc.has_value()) {
            since = to_iso8601z(*options.start_utc);
        }
        if (options.end_exclusive_utc.has_value()) {
            until = to_iso8601z(*options.end_exclusive_utc - std::chrono::seconds{ 1 });
        }
    }
    if (since.empty() && min_time.has_value()) {
        since = to_iso8601z(*min_time);
    }
    if (until.empty() && max_time.has_value()) {
        until = to_iso8601z(*max_time);
    }

    double minutes = 1.0;
    if (options.start_utc.has_value() && options.end_exclusive_utc.has_value()) {
        minutes = std::max(1.0, std::chrono::duration<double>(*options.end_exclusive_utc - *options.start_utc).count() /
                                    60.0);
    } else if (min_time.has_value() && max_time.has_value() && *max_time >= *min_time) {
        minutes = std::max(1.0, std::chrono::duration<double>(*max_time - *min_time).count() / 60.0 + 1.0 / 60.0);
    }

    boost::json::object window;
    window["window"] = "custom";
    window["since"] = since;
    window["until"] = until;
    window["requests"] = requests;
    window["tokens"] = tokens;
    window["rpm"] = static_cast<long long>(std::llround(static_cast<double>(requests) / minutes));
    window["tpm"] = static_cast<long long>(std::llround(static_cast<double>(tokens) / minutes));
    window["input_tokens"] = input_tokens;
    window["output_tokens"] = output_tokens;
    window["cache_read_tokens"] = cache_read_tokens;
    window["cache_creation_tokens"] = cache_creation_tokens;
    window["cache_ratio"] = input_tokens > 0 ? static_cast<double>(cache_read_tokens + cache_creation_tokens) /
                                                   static_cast<double>(input_tokens) :
                                               0.0;
    window["first_token_samples"] = first_token_samples;
    window["avg_first_token_latency"] =
        first_token_samples > 0 ? static_cast<double>(first_token_sum) / static_cast<double>(first_token_samples) : 0.0;
    window["tokens_per_second"] =
        decode_latency_ms > 0 ? static_cast<double>(decode_tokens) * 1000.0 / static_cast<double>(decode_latency_ms) :
                                0.0;
    const std::string committed_usd = request_detail::decimal_to_string(committed);
    window["used_usd"] = committed_usd;
    window["committed_usd"] = committed_usd;
    window["limit_usd"] = request_detail::decimal_to_string(balance_usd);
    window["remaining_usd"] = request_detail::decimal_to_string(balance_usd - committed);
    return window;
}

boost::json::array usage_time_series(const std::vector<Request> &rows, const std::string &tz,
                                     std::string_view granularity)
{
    struct Acc {
        long long requests = 0;
        long long tokens = 0;
        double committed_usd = 0.0;
        long long input = 0;
        long long cached = 0;
        long long first_token_total = 0;
        long long first_token_count = 0;
        long long output = 0;
        long long latency_total = 0;
    };
    std::map<std::string, Acc> buckets;
    for (const Request &req : rows) {
        if (req.time.empty()) {
            continue;
        }
        sys_seconds tp;
        try {
            tp = parse_mysql_datetime(req.time);
        } catch (const std::exception &) {
            continue;
        }
        const std::string bucket = granularity == "day" ? day_bucket(tp, tz) : hour_bucket(tp, tz);
        Acc &acc = buckets[bucket];
        ++acc.requests;
        const long long cache_creation = req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
        acc.tokens += req.input_tokens + req.output_tokens + req.cache_read_tokens + cache_creation;
        acc.input += req.input_tokens;
        acc.cached += req.cache_read_tokens + cache_creation;
        if (req.status == "committed") {
            acc.committed_usd += req.solve_price();
        }
        if (req.first_token_latency_ms > 0) {
            acc.first_token_total += req.first_token_latency_ms;
            ++acc.first_token_count;
        }
        if (req.latency_ms > 0) {
            acc.output += req.output_tokens;
            acc.latency_total += req.latency_ms;
        }
    }

    boost::json::array points;
    for (const auto &[bucket, acc] : buckets) {
        boost::json::object point;
        point["bucket"] = bucket;
        point["requests"] = acc.requests;
        point["tokens"] = acc.tokens;
        point["committed_usd"] = acc.committed_usd;
        point["cache_ratio"] = acc.input > 0 ? static_cast<double>(acc.cached) / static_cast<double>(acc.input) : 0.0;
        point["avg_first_token_latency"] = acc.first_token_count > 0 ? static_cast<double>(acc.first_token_total) /
                                                                           static_cast<double>(acc.first_token_count) :
                                                                       0.0;
        point["tokens_per_second"] =
            acc.latency_total > 0 ? static_cast<double>(acc.output) * 1000.0 / static_cast<double>(acc.latency_total) :
                                    0.0;
        points.push_back(std::move(point));
    }
    return points;
}

boost::json::array dashboard_model_stats(const std::vector<Request> &rows)
{
    struct Acc {
        long long requests = 0;
        long long tokens = 0;
        double committed = 0.0;
    };
    std::map<std::string, Acc> by_model;
    for (const Request &req : rows) {
        const std::string model = req.model.name;
        Acc &acc = by_model[model];
        ++acc.requests;
        acc.tokens += req.input_tokens + req.output_tokens + req.cache_read_tokens + req.cache_creation_5m_tokens +
                      req.cache_creation_1h_tokens;
        if (req.status == "committed") {
            acc.committed += req.solve_price();
        }
    }
    std::vector<std::pair<std::string, Acc>> ranked(by_model.begin(), by_model.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        if (a.second.requests != b.second.requests) {
            return a.second.requests > b.second.requests;
        }
        return a.first < b.first;
    });
    if (ranked.size() > 12) {
        ranked.resize(12);
    }
    static constexpr const char *kColors[] = { "#3b82f6", "#22c55e", "#f59e0b", "#ef4444", "#8b5cf6", "#06b6d4" };
    boost::json::array out;
    for (size_t i = 0; i < ranked.size(); ++i) {
        boost::json::object o;
        o["model"] = ranked[i].first;
        const std::vector<Model> &models = ModelManager::instance().models();
        const auto it =
            std::find_if(models.begin(), models.end(), [&](const Model &m) { return m.name == ranked[i].first; });
        const std::string icon = it != models.end() ? model_icon_url(it->owned_by) : "";
        if (icon.empty()) {
            o["icon_url"] = nullptr;
        } else {
            o["icon_url"] = icon;
        }
        o["color"] = kColors[i % (sizeof(kColors) / sizeof(kColors[0]))];
        o["requests"] = ranked[i].second.requests;
        o["tokens"] = ranked[i].second.tokens;
        o["committed_usd"] = request_detail::decimal_to_string(ranked[i].second.committed);
        out.push_back(std::move(o));
    }
    return out;
}

HttpResponse user_models_detail_http_response(std::string_view raw_request, const Config &config,
                                              std::string_view request_id)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    boost::json::array models_json;
    for (const Model &model : ModelManager::instance().models()) {
        boost::json::object o;
        o["id"] = model.id;
        o["public_id"] = model.name;
        o["group_name"] = "";
        o["owned_by"] = model.owned_by;
        o["input_usd_per_1m"] = request_detail::price_string(model.input_price);
        o["output_usd_per_1m"] = request_detail::price_string(model.output_price);
        o["cache_read_input_usd_per_1m"] = request_detail::price_string(model.cache_read_price);
        o["cache_creation_input_usd_per_1m"] = request_detail::price_string(model.cache_creation_5m_price);
        o["cache_creation_1h_input_usd_per_1m"] = request_detail::price_string(model.cache_creation_1h_price);
        o["status"] = 1;
        o["icon_url"] = model_icon_url(model.owned_by);
        models_json.push_back(std::move(o));
    }
    return api_json_response(api_success(std::move(models_json)), request_id);
}

HttpResponse dashboard_http_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                     std::string_view target)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return api_json_response(api_failure(message), request_id);
    }

    const auto now = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto local = date::make_zoned(options.time_zone, now).get_local_time();
    const date::year_month_day ymd{ date::floor<date::days>(local) };
    int year = static_cast<int>(ymd.year());
    unsigned month = static_cast<unsigned>(ymd.month());
    unsigned day = static_cast<unsigned>(ymd.day());
    options.all_time = false;
    options.start_utc = local_date_to_utc(year, month, day, options.time_zone);
    next_date(year, month, day);
    options.end_exclusive_utc = local_date_to_utc(year, month, day, options.time_zone);

    try {
        auto db = make_database(config.db_dsn);
        RequestStore store(*db);
        UserStore users(*db);
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        const boost::json::object today = aggregate_window(rows, options, users.get_user_balance_usd(user->id));
        boost::json::object charts;
        charts["model_stats"] = dashboard_model_stats(rows);
        charts["time_series_stats"] = usage_time_series(rows, options.time_zone, "hour");
        boost::json::object body;
        body["today_usage_usd"] = today.at("committed_usd");
        body["today_since"] = today.at("since");
        body["today_until"] = today.at("until");
        body["today_requests"] = today.at("requests");
        body["today_tokens"] = today.at("tokens");
        body["today_rpm"] = std::to_string(today.at("rpm").to_number<long long>());
        body["today_tpm"] = std::to_string(today.at("tpm").to_number<long long>());
        body["charts"] = std::move(charts);
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse usage_windows_http_response(std::string_view raw_request, const Config &config,
                                         std::string_view request_id, std::string_view target)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return api_json_response(api_failure(message), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        RequestStore store(*db);
        UserStore users(*db);
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        boost::json::object body;
        body["time_zone"] = options.time_zone;
        body["now"] = to_iso8601z(date::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        boost::json::array windows;
        windows.push_back(aggregate_window(rows, options, users.get_user_balance_usd(user->id)));
        body["windows"] = std::move(windows);
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse requests_http_response(std::string_view raw_request, const Config &config, std::string_view request_id,
                                    std::string_view target)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return api_json_response(api_failure(message), request_id);
    }

    int limit = 50;
    const std::string limit_raw = trim_ascii(query_param_value(params, "limit"));
    if (!limit_raw.empty()) {
        int parsed = 0;
        if (parse_i32(limit_raw, parsed) && parsed > 0 && parsed <= 100) {
            limit = parsed;
        }
    }
    RequestListFilter filter = filter_from_usage_options(user->id, options);
    const std::string before_id_raw = trim_ascii(query_param_value(params, "before_id"));
    if (!before_id_raw.empty()) {
        long long before_id = 0;
        if (parse_i64(before_id_raw, before_id) && before_id > 0) {
            filter.before_id = before_id;
        }
    }
    const std::string q_model = trim_ascii(query_param_value(params, "q_model"));
    if (!q_model.empty()) {
        filter.model_like = q_model;
    }
    filter.limit = limit + 1;

    try {
        auto db = make_database(config.db_dsn);
        RequestStore store(*db);
        auto loaded = store.query(filter);
        const bool has_extra = static_cast<int>(loaded.size()) > limit;
        if (has_extra) {
            loaded.resize(static_cast<size_t>(limit));
        }
        boost::json::object body;
        boost::json::array events;
        for (const Request &req : loaded) {
            events.push_back(request_to_user_event_json(req));
        }
        body["events"] = std::move(events);
        if (has_extra && !loaded.empty()) {
            body["next_before_id"] = loaded.back().id;
        } else {
            body["next_before_id"] = nullptr;
        }
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse usage_timeseries_http_response(std::string_view raw_request, const Config &config,
                                            std::string_view request_id, std::string_view target)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return api_json_response(api_failure(message), request_id);
    }
    std::string granularity = trim_ascii(query_param_value(params, "granularity"));
    if (granularity.empty()) {
        granularity = "day";
    }
    if (granularity != "hour" && granularity != "day") {
        return api_json_response(api_failure("granularity 无效"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        RequestStore store(*db);
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        boost::json::object body;
        body["time_zone"] = options.time_zone;
        body["start"] = options.start_utc.has_value() ? to_iso8601z(*options.start_utc) : "";
        body["end"] = options.end_exclusive_utc.has_value() ?
                          to_iso8601z(*options.end_exclusive_utc - std::chrono::seconds{ 1 }) :
                          "";
        body["granularity"] = granularity;
        body["points"] = usage_time_series(rows, options.time_zone, granularity);
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse usage_event_detail_http_response(std::string_view raw_request, const Config &config,
                                              std::string_view request_id, long long event_id)
{
    HttpResponse auth_response;
    const auto user = api_authenticated_user(raw_request, config, request_id, auth_response);
    if (!user.has_value()) {
        return auth_response;
    }
    if (event_id <= 0) {
        return api_json_response(api_failure("event_id 无效"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        RequestStore store(*db);
        const auto req = store.get_by_id(event_id);
        if (!req.has_value() || req->user_id != user->id) {
            return api_json_response(api_failure("事件不存在"), request_id);
        }
        boost::json::object body;
        body["event_id"] = req->id;
        body["pricing_breakdown"] = pricing_breakdown_to_json(compute_pricing_breakdown(*req));
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

std::optional<User> api_authenticated_admin(std::string_view raw_request, const Config &config,
                                            std::string_view request_id, HttpResponse &response)
{
    std::string failure;
    bool clear_cookie = false;
    const auto user = authenticated_admin_user(raw_request, config, failure, clear_cookie);
    if (user.has_value()) {
        return user;
    }
    std::vector<Header> headers;
    if (clear_cookie) {
        headers.push_back(Header{ "Set-Cookie", clear_session_cookie_header(raw_request) });
    }
    response = api_json_response(api_failure(failure.empty() ? "未登录" : failure), request_id, headers);
    return std::nullopt;
}

struct AdminUsageRange {
    sys_seconds since_utc{};
    sys_seconds until_utc{};
    std::string start;
    std::string end;
    std::string since_local;
    std::string until_local;
    bool all_time = false;
};

std::optional<AdminUsageRange> resolve_admin_usage_range(odb::database &db,
                                                         const std::map<std::string, std::string> &params,
                                                         sys_seconds now_utc, std::string &error)
{
    error.clear();
    AdminUsageRange out;
    const auto today_local = date::make_zoned(std::string{ kAdminTimeZone }, now_utc).get_local_time();
    const date::year_month_day today_ymd{ date::floor<date::days>(today_local) };
    const std::string today = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d");

    const std::string all_time_raw = query_param_value(params, "all_time");
    if (!all_time_raw.empty() && !parse_bool_flag(all_time_raw, out.all_time)) {
        error = "all_time 不合法";
        return std::nullopt;
    }

    std::string start = trim_ascii(query_param_value(params, "start"));
    std::string end = trim_ascii(query_param_value(params, "end"));
    if (out.all_time) {
        RequestStore store(db);
        RequestListFilter filter;
        filter.limit = 1;
        filter.order_asc = true;
        const auto first_rows = store.query(filter);
        if (!first_rows.empty() && !first_rows.front().time.empty()) {
            try {
                start = format_local(parse_mysql_datetime(first_rows.front().time), std::string{ kAdminTimeZone },
                                     "%Y-%m-%d");
                end = today;
            } catch (const std::exception &) {
                start.clear();
                end.clear();
            }
        } else {
            start.clear();
            end.clear();
        }
    }
    if (start.empty()) {
        start = today;
    }
    if (end.empty()) {
        end = start;
    }

    int start_y = 0;
    int start_m = 0;
    int start_d = 0;
    int end_y = 0;
    int end_m = 0;
    int end_d = 0;
    if (!parse_date_yyyy_mm_dd(start, start_y, start_m, start_d)) {
        error = "start 不合法（格式：YYYY-MM-DD）";
        return std::nullopt;
    }
    if (!parse_date_yyyy_mm_dd(end, end_y, end_m, end_d)) {
        error = "end 不合法（格式：YYYY-MM-DD）";
        return std::nullopt;
    }
    unsigned sm = static_cast<unsigned>(start_m);
    unsigned sd = static_cast<unsigned>(start_d);
    unsigned em = static_cast<unsigned>(end_m);
    unsigned ed = static_cast<unsigned>(end_d);
    out.since_utc = local_date_to_utc(start_y, sm, sd, std::string{ kAdminTimeZone });
    const sys_seconds end_start = local_date_to_utc(end_y, em, ed, std::string{ kAdminTimeZone });
    next_date(end_y, em, ed);
    const sys_seconds end_exclusive = local_date_to_utc(end_y, em, ed, std::string{ kAdminTimeZone });
    if (out.since_utc >= end_exclusive) {
        error = "start 不能晚于 end";
        return std::nullopt;
    }
    out.start = start;
    out.end = end;
    out.since_local = format_local(out.since_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d %H:%M");
    const sys_seconds today_start =
        local_date_to_utc(static_cast<int>(today_ymd.year()), static_cast<unsigned>(today_ymd.month()),
                          static_cast<unsigned>(today_ymd.day()), std::string{ kAdminTimeZone });
    if (end_start >= today_start) {
        out.end = today;
        out.until_utc = now_utc;
        out.until_local = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d %H:%M");
    } else {
        out.until_utc = end_exclusive;
        out.until_local =
            format_local(end_exclusive - std::chrono::seconds{ 1 }, std::string{ kAdminTimeZone }, "%Y-%m-%d %H:%M");
    }
    return out;
}

RequestListFilter build_admin_filter(const std::map<std::string, std::string> &params, const AdminUsageRange &range,
                                     int limit, std::string &error, odb::database &db)
{
    error.clear();
    RequestListFilter filters;
    filters.limit = limit;
    filters.start = to_mysql_datetime(range.since_utc);
    filters.end_exclusive = to_mysql_datetime(range.until_utc);

    const std::string user_id_raw = trim_ascii(query_param_value(params, "user_id"));
    if (!user_id_raw.empty()) {
        long long user_id = 0;
        if (!parse_i64(user_id_raw, user_id) || user_id <= 0) {
            error = "user_id 不合法";
            return filters;
        }
        filters.user_id = user_id;
    }
    const std::string channel_id_raw = trim_ascii(query_param_value(params, "channel_id"));
    if (!channel_id_raw.empty()) {
        long long channel_id = 0;
        if (!parse_i64(channel_id_raw, channel_id) || channel_id <= 0) {
            error = "channel_id 不合法";
            return filters;
        }
        filters.channel_id = channel_id;
    }
    const std::string model = trim_ascii(query_param_value(params, "model"));
    if (!model.empty()) {
        filters.model_exact = model;
    }
    const std::string q_model = trim_ascii(query_param_value(params, "q_model"));
    if (!q_model.empty()) {
        filters.model_like = q_model;
    }

    const std::string q_user = trim_ascii(query_param_value(params, "q_user"));
    if (!q_user.empty()) {
        using uq = odb::query<User>;
        ScopedTransaction t(db);
        for (const User &u :
             db.query<User>(uq::email.like("%" + q_user + "%") || uq::username.like("%" + q_user + "%"))) {
            filters.user_ids.push_back(u.id);
        }
        t.commit();
        if (filters.user_ids.empty()) {
            filters.user_ids.push_back(-1);
        }
    }
    const std::string q_channel = trim_ascii(query_param_value(params, "q_channel"));
    if (!q_channel.empty()) {
        using cq = odb::query<Channel>;
        ScopedTransaction t(db);
        for (const Channel &c : db.query<Channel>(cq::name.like("%" + q_channel + "%"))) {
            filters.channel_ids.push_back(c.id);
        }
        t.commit();
        if (filters.channel_ids.empty()) {
            filters.channel_ids.push_back(-1);
        }
    }

    const std::string before_id_raw = trim_ascii(query_param_value(params, "before_id"));
    if (!before_id_raw.empty()) {
        long long before_id = 0;
        if (!parse_i64(before_id_raw, before_id) || before_id <= 0) {
            error = "before_id 不合法";
            return filters;
        }
        filters.before_id = before_id;
    }
    const std::string after_id_raw = trim_ascii(query_param_value(params, "after_id"));
    if (!after_id_raw.empty()) {
        long long after_id = 0;
        if (!parse_i64(after_id_raw, after_id) || after_id <= 0) {
            error = "after_id 不合法";
            return filters;
        }
        filters.after_id = after_id;
        filters.order_asc = true;
    }
    if (filters.before_id.has_value() && filters.after_id.has_value()) {
        error = "before_id 与 after_id 不能同时使用";
    }
    return filters;
}

std::string state_label(std::string_view status)
{
    if (status == "pending") {
        return "处理中";
    }
    if (status == "committed") {
        return "已结算";
    }
    if (status == "void") {
        return "已作废";
    }
    if (status == "expired") {
        return "已过期";
    }
    return std::string{ status };
}

std::string state_badge_class(std::string_view status)
{
    if (status == "pending") {
        return "bg-warning-subtle text-warning border border-warning-subtle";
    }
    if (status == "committed") {
        return "bg-success-subtle text-success border border-success-subtle";
    }
    return "bg-secondary-subtle text-secondary border border-secondary-subtle";
}

boost::json::object request_to_admin_event_json(const Request &req, std::string_view user_email,
                                                std::string_view channel_name)
{
    const long long cached_tokens = req.cache_read_tokens + req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
    const std::string tps = (req.output_tokens > 0 && req.latency_ms > 0) ?
                                request_detail::decimal_to_string(static_cast<double>(req.output_tokens) * 1000.0 /
                                                                  static_cast<double>(req.latency_ms)) :
                                "-";
    const std::string status = status_json_label(req.status);
    boost::json::object o;
    o["id"] = req.id;
    o["time"] = mysql_to_iso_utc(req.time);
    o["user_id"] = req.user_id;
    o["user_email"] = user_email;
    o["endpoint"] = nullable_odb_string(req.endpoint).value_or("");
    o["method"] = nullable_odb_string(req.method).value_or("");
    o["model"] = req.model.name;
    o["status_code"] = std::to_string(req.status_code);
    o["latency_ms"] = std::to_string(req.latency_ms);
    o["first_token_latency_ms"] = std::to_string(req.first_token_latency_ms);
    o["tokens_per_second"] = tps;
    o["input_tokens"] = std::to_string(req.input_tokens);
    o["output_tokens"] = std::to_string(req.output_tokens);
    o["cached_tokens"] = cached_tokens > 0 ? std::to_string(cached_tokens) : "-";
    const std::string committed = request_detail::decimal_to_string(req.solve_price());
    o["cost_usd"] = committed;
    o["committed_usd"] = committed;
    o["status"] = status;
    o["state_label"] = state_label(status);
    o["state_badge_class"] = state_badge_class(status);
    const auto service_tier = nullable_odb_string(req.service_tier);
    if (service_tier.has_value()) {
        o["service_tier"] = *service_tier;
    } else {
        o["service_tier"] = nullptr;
    }
    o["is_stream"] = req.is_stream;
    o["channel_id"] = req.channel_id > 0 ? std::to_string(req.channel_id) : "-";
    o["upstream_channel_name"] = channel_name;
    o["request_id"] = std::to_string(req.id);
    const auto error_class = nullable_odb_string(req.error_class);
    const auto error_message = nullable_odb_string(req.error_message);
    std::string error;
    if (error_class.has_value() && error_message.has_value()) {
        error = *error_class + " (" + *error_message + ")";
    } else if (error_class.has_value()) {
        error = *error_class;
    } else if (error_message.has_value()) {
        error = *error_message;
    }
    o["error"] = error;
    o["error_class"] = error_class.value_or("");
    o["error_message"] = error_message.value_or("");
    return o;
}

boost::json::object admin_window_summary(const AdminUsageRange &range, const std::vector<Request> &rows,
                                         const std::vector<Request> &recent_rows)
{
    long long requests = 0;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_tokens = 0;
    long long cache_creation_tokens = 0;
    long long first_token_sum = 0;
    long long first_token_samples = 0;
    long long decode_tokens = 0;
    long long decode_latency_ms = 0;
    double committed = 0.0;
    for (const Request &req : rows) {
        if (req.status != "committed") {
            continue;
        }
        ++requests;
        input_tokens += req.input_tokens;
        output_tokens += req.output_tokens;
        cache_read_tokens += req.cache_read_tokens;
        cache_creation_tokens += req.cache_creation_5m_tokens + req.cache_creation_1h_tokens;
        committed += req.solve_price();
        if (req.first_token_latency_ms > 0) {
            first_token_sum += req.first_token_latency_ms;
            ++first_token_samples;
        }
        if (req.output_tokens > 0 && req.latency_ms > req.first_token_latency_ms) {
            decode_tokens += req.output_tokens;
            decode_latency_ms += req.latency_ms - req.first_token_latency_ms;
        }
    }
    long long recent_requests = 0;
    long long recent_tokens = 0;
    for (const Request &req : recent_rows) {
        if (req.status != "committed") {
            continue;
        }
        ++recent_requests;
        recent_tokens += req.input_tokens + req.output_tokens;
    }
    const double total_tokens = static_cast<double>(input_tokens + output_tokens);
    const double cached_tokens = static_cast<double>(cache_read_tokens + cache_creation_tokens);
    boost::json::object o;
    o["window"] = "统计区间";
    o["since"] = range.since_local;
    o["until"] = range.until_local;
    o["requests"] = requests;
    o["tokens"] = input_tokens + output_tokens;
    o["input_tokens"] = input_tokens;
    o["output_tokens"] = output_tokens;
    o["cached_tokens"] = cache_read_tokens + cache_creation_tokens;
    o["cache_ratio"] =
        request_detail::decimal_to_string((total_tokens > 0 ? cached_tokens / total_tokens : 0.0) * 100.0);
    o["rpm"] = request_detail::decimal_to_string(static_cast<double>(recent_requests));
    o["tpm"] = request_detail::decimal_to_string(static_cast<double>(recent_tokens));
    o["avg_first_token_latency"] = request_detail::decimal_to_string(
        first_token_samples > 0 ? static_cast<double>(first_token_sum) / static_cast<double>(first_token_samples) :
                                  0.0);
    o["tokens_per_second"] = request_detail::decimal_to_string(
        decode_latency_ms > 0 ? static_cast<double>(decode_tokens) * 1000.0 / static_cast<double>(decode_latency_ms) :
                                0.0);
    o["committed_usd"] = request_detail::decimal_to_string(committed);
    return o;
}

boost::json::array top_users_json(odb::database &db, const std::vector<Request> &rows)
{
    struct Acc {
        std::string email;
        std::string role;
        long long status = 0;
        double committed = 0.0;
    };
    std::map<long long, Acc> by_user;
    UserStore users(db);
    for (const Request &req : rows) {
        if (req.status != "committed") {
            continue;
        }
        Acc &acc = by_user[req.user_id];
        if (acc.email.empty()) {
            const User u = users.get_user_by_id(req.user_id);
            acc.email = u.email;
            acc.role = u.role;
            acc.status = u.status;
        }
        acc.committed += req.solve_price();
    }
    std::vector<std::pair<long long, Acc>> ranked(by_user.begin(), by_user.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        if (a.second.committed != b.second.committed) {
            return a.second.committed > b.second.committed;
        }
        return a.first > b.first;
    });
    if (ranked.size() > 50) {
        ranked.resize(50);
    }
    boost::json::array out;
    for (const auto &entry : ranked) {
        boost::json::object o;
        o["user_id"] = entry.first;
        o["email"] = entry.second.email;
        o["role"] = entry.second.role;
        o["status"] = entry.second.status;
        o["committed_usd"] = request_detail::decimal_to_string(entry.second.committed);
        out.push_back(std::move(o));
    }
    return out;
}

HttpResponse admin_dashboard_http_response(std::string_view raw_request, const Config &config,
                                           std::string_view request_id)
{
    HttpResponse auth_response;
    if (!api_authenticated_admin(raw_request, config, request_id, auth_response)) {
        return auth_response;
    }
    try {
        auto db = make_database(config.db_dsn);
        UserStore users(*db);
        ChannelStore channels(*db);
        const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto local = date::make_zoned(std::string{ kAdminTimeZone }, now_utc).get_local_time();
        const date::year_month_day ymd{ date::floor<date::days>(local) };
        const sys_seconds today_start =
            local_date_to_utc(static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                              static_cast<unsigned>(ymd.day()), std::string{ kAdminTimeZone });

        RequestListFilter filter;
        filter.start = to_mysql_datetime(today_start);
        filter.end_exclusive = to_mysql_datetime(now_utc);
        RequestStore store(*db);
        const auto rows = store.query(filter);
        long long requests_today = 0;
        long long input_tokens = 0;
        long long output_tokens = 0;
        double cost = 0.0;
        for (const Request &req : rows) {
            if (req.status != "committed") {
                continue;
            }
            ++requests_today;
            input_tokens += req.input_tokens;
            output_tokens += req.output_tokens;
            cost += req.solve_price();
        }
        boost::json::object stats;
        stats["users_count"] = users.count_users();
        const auto channel_list = channels.list_channels();
        stats["channels_count"] = static_cast<long long>(channel_list.size());
        stats["endpoints_count"] = static_cast<long long>(channel_list.size());
        stats["requests_today"] = requests_today;
        stats["tokens_today"] = input_tokens + output_tokens;
        stats["input_tokens_today"] = input_tokens;
        stats["output_tokens_today"] = output_tokens;
        stats["cost_today"] = request_detail::decimal_to_string(cost);
        boost::json::object data;
        data["admin_time_zone"] = kAdminTimeZone;
        data["stats"] = std::move(stats);
        return api_json_response(api_success(std::move(data)), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("读取统计失败"), request_id);
    }
}

HttpResponse admin_usage_page_http_response(std::string_view raw_request, const Config &config,
                                            std::string_view request_id, std::string_view target)
{
    HttpResponse auth_response;
    if (!api_authenticated_admin(raw_request, config, request_id, auth_response)) {
        return auth_response;
    }
    const auto params = parse_query_map(target);
    int limit = 50;
    const std::string limit_raw = query_param_value(params, "limit");
    if (!limit_raw.empty() && !parse_i32(limit_raw, limit)) {
        return api_json_response(api_failure("limit 不合法"), request_id);
    }
    if (limit < 10) {
        limit = 10;
    }
    if (limit > 200) {
        limit = 200;
    }
    bool include_summary = true;
    const std::string summary_raw = query_param_value(params, "summary");
    if (!summary_raw.empty() && !parse_bool_flag(summary_raw, include_summary)) {
        return api_json_response(api_failure("summary 不合法"), request_id);
    }

    try {
        auto db = make_database(config.db_dsn);
        const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        std::string range_error;
        const auto range = resolve_admin_usage_range(*db, params, now_utc, range_error);
        if (!range.has_value()) {
            return api_json_response(api_failure(range_error), request_id);
        }
        std::string filter_error;
        RequestListFilter page_filter = build_admin_filter(params, *range, limit + 1, filter_error, *db);
        if (!filter_error.empty()) {
            return api_json_response(api_failure(filter_error), request_id);
        }
        RequestStore store(*db);
        auto loaded = store.query(page_filter);
        const bool after = page_filter.after_id.has_value();
        if (after) {
            std::reverse(loaded.begin(), loaded.end());
        }
        const bool has_extra = static_cast<int>(loaded.size()) > limit;
        if (has_extra) {
            loaded.resize(static_cast<size_t>(limit));
        }

        std::map<long long, std::string> emails;
        std::map<long long, std::string> channel_names;
        UserStore users(*db);
        ChannelStore channels(*db);
        for (const Channel &c : channels.list_channels()) {
            channel_names[c.id] = c.name;
        }

        boost::json::array events;
        for (const Request &req : loaded) {
            if (!emails.contains(req.user_id)) {
                emails[req.user_id] = users.get_user_by_id(req.user_id).email;
            }
            events.push_back(request_to_admin_event_json(
                req, emails[req.user_id], channel_names.contains(req.channel_id) ? channel_names[req.channel_id] : ""));
        }

        boost::json::object data;
        data["admin_time_zone"] = kAdminTimeZone;
        data["now"] = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d %H:%M");
        data["start"] = range->start;
        data["end"] = range->end;
        data["limit"] = limit;
        data["events"] = std::move(events);
        if (has_extra && !loaded.empty()) {
            data["next_before_id"] = loaded.back().id;
        } else {
            data["next_before_id"] = nullptr;
        }
        if ((after || page_filter.before_id.has_value()) && !loaded.empty()) {
            data["prev_after_id"] = loaded.front().id;
        } else {
            data["prev_after_id"] = nullptr;
        }
        data["cursor_active"] = page_filter.before_id.has_value() || page_filter.after_id.has_value();

        if (include_summary) {
            RequestListFilter summary_filter = page_filter;
            summary_filter.limit = 0;
            summary_filter.before_id.reset();
            summary_filter.after_id.reset();
            summary_filter.order_asc = false;
            const auto summary_rows = store.query(summary_filter);
            RequestListFilter recent_filter;
            recent_filter.start = to_mysql_datetime(now_utc - std::chrono::seconds{ 60 });
            recent_filter.end_exclusive = to_mysql_datetime(now_utc + std::chrono::seconds{ 1 });
            const auto recent_rows = store.query(recent_filter);
            data["window"] = admin_window_summary(*range, summary_rows, recent_rows);
            data["top_users"] = top_users_json(*db, summary_rows);
        }
        return api_json_response(api_success(std::move(data)), request_id);
    } catch (const std::exception &err) {
        return api_json_response(api_failure(err.what()), request_id);
    }
}

HttpResponse admin_usage_event_detail_http_response(std::string_view raw_request, const Config &config,
                                                    std::string_view request_id, long long event_id)
{
    HttpResponse auth_response;
    if (!api_authenticated_admin(raw_request, config, request_id, auth_response)) {
        return auth_response;
    }
    if (event_id <= 0) {
        return api_json_response(api_failure("event_id 不合法"), request_id);
    }
    try {
        auto db = make_database(config.db_dsn);
        RequestStore store(*db);
        const auto req = store.get_by_id(event_id);
        if (!req.has_value()) {
            return api_json_response(api_failure("not found"), request_id);
        }
        boost::json::object body;
        body["event_id"] = req->id;
        body["pricing_breakdown"] = pricing_breakdown_to_json(compute_pricing_breakdown(*req));
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询失败"), request_id);
    }
}

HttpResponse admin_usage_timeseries_http_response(std::string_view raw_request, const Config &config,
                                                  std::string_view request_id, std::string_view target)
{
    HttpResponse auth_response;
    if (!api_authenticated_admin(raw_request, config, request_id, auth_response)) {
        return auth_response;
    }
    std::map<std::string, std::string> params = parse_query_map(target);
    std::string granularity = lowercase_ascii(trim_ascii(query_param_value(params, "granularity")));
    if (granularity.empty()) {
        granularity = "hour";
    }
    if (granularity != "hour" && granularity != "day") {
        return api_json_response(api_failure("granularity 仅支持 hour/day"), request_id);
    }
    bool all_time = false;
    const std::string all_time_raw = query_param_value(params, "all_time");
    if (!all_time_raw.empty() && !parse_bool_flag(all_time_raw, all_time)) {
        return api_json_response(api_failure("all_time 不合法"), request_id);
    }
    const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    if (query_param_value(params, "start").empty() && query_param_value(params, "end").empty() && !all_time) {
        if (granularity == "day") {
            params["start"] = format_local(now_utc - std::chrono::seconds{ 29 * 24 * 3600 },
                                           std::string{ kAdminTimeZone }, "%Y-%m-%d");
            params["end"] = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d");
        } else {
            params["start"] = format_local(now_utc, std::string{ kAdminTimeZone }, "%Y-%m-%d");
            params["end"] = params["start"];
        }
    }
    try {
        auto db = make_database(config.db_dsn);
        std::string range_error;
        const auto range = resolve_admin_usage_range(*db, params, now_utc, range_error);
        if (!range.has_value()) {
            return api_json_response(api_failure(range_error), request_id);
        }
        std::string filter_error;
        RequestListFilter filters = build_admin_filter(params, *range, 0, filter_error, *db);
        if (!filter_error.empty()) {
            return api_json_response(api_failure(filter_error), request_id);
        }
        RequestStore store(*db);
        const auto rows = store.query(filters);
        boost::json::object body;
        body["admin_time_zone"] = kAdminTimeZone;
        body["start"] = range->start;
        body["end"] = range->end;
        body["granularity"] = granularity;
        body["points"] = usage_time_series(rows, std::string{ kAdminTimeZone }, granularity);
        return api_json_response(api_success(std::move(body)), request_id);
    } catch (const std::exception &) {
        return api_json_response(api_failure("查询失败"), request_id);
    }
}
