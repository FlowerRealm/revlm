#pragma once

/*
 * HTTP transport helpers shared by the core and by protocol plugins.
 *
 * What lives here is everything the old `gateway.hpp` carried that is *not*
 * protocol knowledge: hop-by-hop header hygiene, forwarded-header construction,
 * request/response correlation, raw-socket writing and stream draining. A
 * plugin needs these to speak HTTP correctly; none of them knows what an SSE
 * event, a usage object or a token is.
 *
 * The protocol-shaped half of the old gateway -- SSE framing, usage parsing,
 * pricing, the candidate loop -- is gone. The loop moved to
 * proxy/protocol_dispatch.hpp (ADR 0009); the rest moved into the plugins that
 * actually understand those formats.
 */

#include <httplib.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "proxy/upstream.hpp"
#include "request/proxy_request.hpp"
#include "util/json.hpp"

namespace revlm
{

/* Proxy exit: pass an upstream status/body/headers through to the client. */
void write_upstream(::httplib::Response &res, int status, std::string body,
                    const std::vector<UpstreamHeader> &headers = {});

/*
 * Build the upstream request for a proxied call: forwarded headers derived from
 * a trusted-proxy check, hop-by-hop headers dropped, and client credentials
 * stripped so a caller's key can never reach an upstream.
 */
UpstreamRequest build_proxy_upstream_request(const ProxyRequest &pr, std::string_view path);

/* Correlation between the client's request id and the upstream's response id. */
std::string upstream_response_id_from_headers(const std::vector<UpstreamHeader> &headers);
void set_stream_correlation_headers(::httplib::Response &res, std::string_view response_id);
std::vector<UpstreamHeader> merge_correlation_headers(const std::vector<UpstreamHeader> &upstream_headers,
                                                      std::string_view response_id);

/* Non-null when the user cannot pay: the JSON error body to return. */
std::optional<json> paygo_balance_gate(long long user_id);

/*
 * Read an upstream stream to the end. Used when a reply arrived on the
 * streaming path but turned out to be an error the handler must read whole.
 */
std::string read_remaining_stream(const UpstreamReadHandle &stream);

} // namespace revlm
