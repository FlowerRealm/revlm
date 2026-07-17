#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace revlm
{

struct GatewayStreamPump {
    bool completed = false;
    bool client_disconnected = false;
    bool idle_timeout = false;
    bool upstream_error = false;
    bool saw_usage = false;
    size_t response_bytes = 0;
    int first_token_latency_ms = 0;
    std::optional<std::string> model;
};

} // namespace revlm
