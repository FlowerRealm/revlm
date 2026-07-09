#pragma once

#include <string>
#include <string_view>

#include "models/models.hpp"
#include "store/mysql.hpp"

namespace revlm
{

class Request {
public:
    Request();
    Request(Model model, int input_tokens, int output_tokens, int cache_read_tokens, int cache_creation_1h_tokens,
            int cache_creation_5m_tokens, double tier_multiplier = 1.0, double channel_multiplier = 1.0)
        : model(model)
        , input_tokens(input_tokens)
        , output_tokens(output_tokens)
        , cache_read_tokens(cache_read_tokens)
        , cache_creation_1h_tokens(cache_creation_1h_tokens)
        , cache_creation_5m_tokens(cache_creation_5m_tokens)
        , tier_multiplier(tier_multiplier)
        , channel_multiplier(channel_multiplier)
    {
    }
    Model model;
    long long id = 0;
    std::string time;
    long long user_id;
    std::string endpoint;
    std::string method;
    long long token_id = 0;
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;
    int cache_creation_1h_tokens = 0;
    int cache_creation_5m_tokens = 0;
    double tier_multiplier = 1.0;
    std::string service_tier;
    double channel_multiplier = 1.0;
    long long channel_id;
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    std::string error_class;
    std::string error_message;
    bool is_stream = false;
    bool statue;
    double solve_price() const;
    bool commit_usage_event(MysqlConnection &conn, std::string_view finished_at) const;

private:
};

std::string request_timestamp_now();

} // namespace revlm
