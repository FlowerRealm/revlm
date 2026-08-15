#pragma once

#include <string>
#include <utility>
#include <vector>

#include "util/json.hpp"

namespace revlm
{

/*
 * One client request as the core sees it.
 *
 * Flat on purpose. The five nested structs this replaces (HttpRequest, Auth,
 * Pricing, Usage, Upstream) grouped fields by which part of the old Gateway
 * touched them, which is not a distinction anyone reading a request record
 * cares about, and reaching through `pr.upstream.pricing.input_price` said
 * nothing that `pr` could not say directly.
 *
 * The test for membership is whether the CORE aggregates or filters on a field,
 * not whether some protocol happens to have the concept. Token counts, cache
 * tiers and service tiers are protocol-shaped and now live inside
 * `usage_details`, which the core stores and never parses.
 */
struct ProxyRequest {
    long long id = 0;
    std::string request_id;
    std::string time;

    /* Inbound HTTP. Headers are stripped of authorization/x-api-key. */
    std::string method;
    std::string path;
    std::string body;
    std::string client_ip;
    std::vector<std::pair<std::string, std::string>> headers;

    /* Who is paying. */
    long long user_id = 0;
    long long token_id = 0;
    long long channel_group_id = 0;

    /* The upstream attempt that produced the final result. */
    long long channel_id = 0;
    std::string model_name;
    std::string response_id;
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;

    /*
     * Billing. The plugin fills `usage_details` (raw, protocol-shaped, opaque to
     * the core) and `protocol_cost_usd` before returning Done; the core applies
     * `channel_group_multiplier` to reach `usd`. Only the multiplier and `usd`
     * are persisted -- the base amount is recoverable from the two.
     */
    json usage_details;
    double protocol_cost_usd = 0.0;
    double channel_group_multiplier = 1.0;
    double usd = 0.0;

    std::string error_message;
};

} // namespace revlm
