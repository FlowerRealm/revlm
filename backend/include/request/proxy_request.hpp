#pragma once

#include <string>
#include <utility>
#include <vector>

#include "channels/channels.hpp" // complete Channel for the ChannelGroup snapshot vector

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

struct Upstream {
    long long channel_id = 0;
    std::string model_name;
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    std::string response_id;
    // Core-owned multiplier snapshot taken from the ChannelGroup at commit.
    double channel_group_multiplier = 1.0;
};

// Transient, request-scoped snapshot of the ChannelGroup the API key resolved
// to (ADR-0003). Carries the full channel list so the plugin hook can iterate
// candidates without touching the database. Only the opaque object identity and
// the round-robin pointer are owned by the core; everything else is data.
struct ChannelGroupSnapshot {
    long long id = 0;
    std::string type;
    double price_multiplier = 1.0;
    bool status = true;
    std::vector<Channel> channels;
    int pointer = 0;
};

struct ProxyRequest {
    long long id = 0;
    std::string request_id;
    std::string time;
    bool is_stream = false;

    HttpRequest http;
    Auth auth;
    Upstream upstream;
    // ChannelGroup the API key resolved to, parsed by the core before the v1
    // hook is entered. Plugin uses type + channel list for protocol routing and
    // candidate iteration; core owns the round-robin pointer state.
    ChannelGroupSnapshot channel_group;

    // Plugin-owned billing inputs (ADR-0004). token_details holds the complete
    // raw token/usage JSON verbatim (unknown fields preserved); protocol_cost_usd
    // is the runtime base amount computed by the plugin from its protocol
    // semantics and model pricing. The core applies the ChannelGroup multiplier,
    // debits and persists; it never parses token_details.
    std::string token_details;
    double protocol_cost_usd = 0.0;

    std::string error_class;
    std::string error_message;
};

} // namespace revlm
