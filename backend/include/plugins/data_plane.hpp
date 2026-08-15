#pragma once

/*
 * Revlm plugin data-plane interface.
 *
 * Unlike the control plane (plugins/abi.h), this layer is plain C++ and is
 * coupled to the host's C++ ABI: a plugin is built against one Revlm and
 * upgrades with it on a cold restart. That coupling buys a streaming path with
 * no serialization per chunk, which is the load Revlm actually carries. See
 * ADR 0008.
 *
 * The shape of a handler follows ADR 0009: the core owns the candidate-rotation
 * loop, so a handler is called once per upstream attempt and answers with a
 * verdict. There is no commit function here -- the core commits exactly once, at
 * the single exit of its own loop, which is why a plugin cannot double-charge.
 */

#include <httplib.h>

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "channels/channels.hpp"
#include "plugins/abi.h"
#include "proxy/upstream.hpp"
#include "request/proxy_request.hpp"

namespace revlm
{

/* Sink for bytes headed to the client. Returns false once the client is gone. */
using ClientWriter = std::function<bool(std::string_view)>;

/*
 * The client end of a streamed response.
 *
 * `alive` exists because `write` only reports a departed client when there is
 * something to write: a client that hangs up while the upstream is silent would
 * otherwise go unnoticed until the upstream idle timeout, holding a connection
 * open for nobody. A pump that waits on its upstream should probe `alive`
 * between waits.
 */
struct ClientSink {
    ClientWriter write;
    std::function<bool()> alive;
};

/*
 * The body half of a streaming response, written by the plugin.
 *
 * It runs after the request handler has returned, because that is when the HTTP
 * server hands out a writable sink -- so a handler cannot push bytes while it is
 * still deciding whether to try another candidate. That ordering is not a
 * limitation to work around: it is exactly the rule that keeps a rotation from
 * happening behind a response already on the wire.
 *
 * The pump is handed the request record to fill in, and must use that reference
 * rather than one captured from AttemptContext. By the time a pump runs, the
 * AttemptContext is gone and the ProxyRequest it referred to lived on the HTTP
 * handler's stack frame, so a captured pointer to it dangles. Passing the object
 * in is what makes writing the usage of a streamed response possible at all.
 */
using StreamPump = std::function<void(const ClientSink &client, ProxyRequest &proxy)>;

/* A streaming response handed back to the core by stream_to_client(). */
struct PendingStream {
    int status = 200;
    std::string content_type;
    std::vector<UpstreamHeader> headers;
    StreamPump pump;
};

/*
 * One upstream reply, streaming or not. `rest` holds a value exactly when the
 * upstream is still open and more bytes are coming; `body` is then the part
 * already read. Merging the two former structs means a handler faces one
 * transport shape instead of picking between two.
 */
struct UpstreamReply {
    int status_code = 0;
    std::vector<UpstreamHeader> headers;
    std::string body;
    std::optional<UpstreamReadHandle> rest;

    bool streaming() const
    {
        return rest.has_value();
    }
};

/* What the handler tells the core when it returns. */
enum class AttemptOutcome {
    /* Final result determined: upstream succeeded, or the error is not recoverable. */
    Done,
    /* Recoverable failure. The core moves to the next candidate Channel. */
    NextCandidate,
};

/*
 * Everything one attempt sees. The object outlives a single attempt -- it is
 * created once per client request and handed to each attempt in turn -- so a
 * handler may carry protocol state across candidates.
 */
struct AttemptContext {
    /*
     * The upstream target the core picked for THIS attempt. A pointer rather
     * than a reference because the core re-seats it on every rotation while the
     * rest of the context stays put; never null inside a handler.
     */
    const Channel *channel = nullptr;
    /* The ChannelGroup.type the route was matched on. */
    std::string_view group_type;
    /* 0 for the first attempt, incremented on every rotation. */
    int attempt = 0;

    const ::httplib::Request &request;
    ::httplib::Response &response;

    /*
     * The core request record under construction. Before returning Done the
     * handler fills `usage_details` (raw, protocol-shaped, never parsed by the
     * core) and `protocol_cost_usd`. The core then applies the ChannelGroup
     * multiplier, debits the balance and persists the row.
     */
    ProxyRequest &proxy;

    /*
     * The streaming response the handler handed over, if any. Plugins fill this
     * through stream_to_client() rather than by assignment; the core installs it
     * once the rotation loop has ended.
     */
    std::optional<PendingStream> pending_stream;
};

/*
 * Hand a streaming response to the core: status line and headers now, body later
 * through `pump`.
 *
 * Calling this ends candidate rotation -- the response head is committed, and
 * splicing a second upstream's output behind it would corrupt the stream. A
 * handler that calls this and then returns NextCandidate is treated as Done and
 * the disagreement is recorded on the request.
 *
 * Nothing is committed at handover: the request record is written when the pump
 * finishes, so a stream that dies halfway still records whatever usage the pump
 * had accumulated by then. Usage discovered during the stream must be written
 * into the ProxyRequest the pump is handed, not into ctx.proxy -- see StreamPump.
 */
void stream_to_client(AttemptContext &ctx, int status, std::string content_type, std::vector<UpstreamHeader> headers,
                      StreamPump pump);

/*
 * Send one request to ctx.channel. SSRF validation and the per-attempt timeout
 * are applied by the core. A transport failure comes back as an error string;
 * an HTTP error status comes back as a normal reply, because whether a given
 * status is recoverable is protocol knowledge the core does not have.
 *
 * With `stream` set the reply's `rest` holds the still-open upstream and `body`
 * is only what has arrived so far; the handler drives the rest itself, since SSE
 * framing is protocol knowledge and the core does not parse it.
 */
std::expected<UpstreamReply, std::string> send_upstream(AttemptContext &ctx, UpstreamRequest request, bool stream);

/* The function a plugin registers for a (method, path, ChannelGroup.type) key. */
using ProtocolHandler = AttemptOutcome (*)(AttemptContext &);

/* An ordinary global endpoint, outside the data plane. */
using EndpointHandler = void (*)(const ::httplib::Request &, ::httplib::Response &);

/*
 * Casts between the typed handler and the generic function pointer the control
 * plane carries. Converting a function pointer to another function pointer type
 * and back is well defined, so this is a cast and nothing more.
 */
inline revlm_plugin_fn erase_handler(ProtocolHandler handler)
{
    return reinterpret_cast<revlm_plugin_fn>(handler);
}

inline revlm_plugin_fn erase_handler(EndpointHandler handler)
{
    return reinterpret_cast<revlm_plugin_fn>(handler);
}

inline ProtocolHandler protocol_handler_from(revlm_plugin_fn fn)
{
    return reinterpret_cast<ProtocolHandler>(fn);
}

inline EndpointHandler endpoint_handler_from(revlm_plugin_fn fn)
{
    return reinterpret_cast<EndpointHandler>(fn);
}

} // namespace revlm
