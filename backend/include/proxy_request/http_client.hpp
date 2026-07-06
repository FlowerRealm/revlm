#pragma once

#include "proxy_request/upstream.hpp"

namespace revlm
{

UpstreamResponse execute_upstream_http_request(const UpstreamPreparedRequest &prepared, int timeout_ms,
                                               bool allow_private_target);

UpstreamStreamResponse execute_upstream_http_stream_request(const UpstreamPreparedRequest &prepared, int timeout_ms,
                                                            bool allow_private_target);

} // namespace revlm
