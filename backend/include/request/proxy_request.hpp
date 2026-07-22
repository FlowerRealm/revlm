#pragma once

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "models/models.hpp"

namespace revlm
{

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    std::string client_ip;
    // Stripped of authorization/x-api-key by make_request.
    // Uses vector<pair<>> to match UpstreamHeader structure.
    std::vector<std::pair<std::string, std::string>> headers;
};

struct Auth {
    long long user_id = 0;
    long long token_id = 0;
    long long channel_group_id = 0;
};

struct Pricing {
    double input_price = 0.0;
    double output_price = 0.0;
    double cache_read_price = 0.0;
    double cache_creation_1h_price = 0.0;
    double cache_creation_5m_price = 0.0;
};

struct Usage {
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;
    int cache_creation_1h_tokens = 0;
    int cache_creation_5m_tokens = 0;
};

struct Upstream {
    long long channel_id = 0;
    std::string model_name;
    std::string service_tier;
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    std::string response_id;
    double channel_multiplier = 1.0;
    double tier_multiplier = 1.0;
    Pricing pricing;
};

struct ProxyRequest {
    long long id = 0;
    std::string request_id;
    std::string time;
    bool is_stream = false;

    HttpRequest http;
    Auth auth;
    Upstream upstream;
    Usage usage;

    std::string error_class;
    std::string error_message;
};

inline void fill_pricing_from_model(Pricing &pricing, const Model &model)
{
    pricing.input_price = model.input_price;
    pricing.output_price = model.output_price;
    pricing.cache_read_price = model.cache_read_price;
    pricing.cache_creation_1h_price = model.cache_creation_1h_price;
    pricing.cache_creation_5m_price = model.cache_creation_5m_price;
}

inline double compute_usd(const ProxyRequest &pr)
{
    if (pr.upstream.tier_multiplier <= 0.0 || pr.upstream.channel_multiplier <= 0.0) {
        std::fprintf(stderr, "compute_usd: invalid multiplier tier=%.4f channel=%.4f\n", pr.upstream.tier_multiplier,
                     pr.upstream.channel_multiplier);
        return 0.0;
    }
    const Pricing &p = pr.upstream.pricing;
    const Usage &u = pr.usage;
    return (p.input_price * u.input_tokens / 1000000.0 + p.output_price * u.output_tokens / 1000000.0 +
            p.cache_read_price * u.cache_read_tokens / 1000000.0 +
            p.cache_creation_1h_price * u.cache_creation_1h_tokens / 1000000.0 +
            p.cache_creation_5m_price * u.cache_creation_5m_tokens / 1000000.0) *
           pr.upstream.tier_multiplier * pr.upstream.channel_multiplier;
}

} // namespace revlm
