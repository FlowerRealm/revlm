#include "proxy/protocol_dispatch.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "channels/channels.hpp"
#include "config/config.hpp"
#include "plugins/registry.hpp"
#include "proxy/upstream.hpp"
#include "server/http_server.hpp"
#include "util/json.hpp"
#include "util/strings.hpp"

namespace revlm
{

namespace
{

/*
 * Every attempt after the first waits a little. The sequence is capped rather
 * than unbounded because the candidate list wraps forever by design: there is no
 * retry limit, so an uncapped backoff would eventually stall a request that a
 * recovering upstream could still serve.
 */
int backoff_delay_ms(int attempt)
{
    const int base = config().gateway_retry_base_delay_ms;
    const int cap = config().gateway_retry_max_delay_ms;
    if (attempt <= 0) {
        return 0;
    }
    long long delay = static_cast<long long>(base) * attempt;
    return static_cast<int>(std::min<long long>(delay, cap));
}

void write_dispatch_error(::httplib::Response &response, int status, std::string_view message)
{
    write_json(response, status, json({ { "success", false }, { "message", std::string{ message } } }));
}

} // namespace

std::expected<UpstreamReply, std::string> send_upstream(AttemptContext &ctx, UpstreamRequest request, bool stream)
{
    if (ctx.channel == nullptr) {
        return std::unexpected(std::string{ "no channel selected for this attempt" });
    }

    const int timeout_ms = config().proxy_upstream_timeout_seconds * 1000;
    const bool allow_private_target = upstream_channel_allows_private_target(*ctx.channel);
    const UpstreamExecutor executor;

    // Transport failures come back as an error string. An HTTP error *status* is
    // not a failure here and is handed back as an ordinary reply -- whether a
    // given status is worth another candidate is protocol knowledge, and the
    // core deciding it would be the exact retry-by-status-code behaviour ADR 0009
    // rules out.
    try {
        if (stream) {
            const UpstreamPreparedRequest prepared =
                executor.prepare(ctx.channel->id, std::move(request), !allow_private_target);
            UpstreamStreamResponse upstream =
                default_upstream_http_stream_transport(prepared, timeout_ms, allow_private_target);
            return UpstreamReply{
                .status_code = upstream.status_code,
                .headers = std::move(upstream.headers),
                .body = std::move(upstream.initial_body),
                .rest = std::move(upstream.stream),
            };
        }
        UpstreamExecutionResult executed = execute_with_default_transport(executor, ctx.channel->id, std::move(request),
                                                                          timeout_ms, allow_private_target);
        return UpstreamReply{
            .status_code = executed.response.status_code,
            .headers = std::move(executed.response.headers),
            .body = std::move(executed.response.body),
            .rest = std::nullopt,
        };
    } catch (const std::invalid_argument &) {
        // Distinguished from a connect failure because a malformed base URL is a
        // configuration mistake on this channel, not a transient outage.
        return std::unexpected(std::string{ "upstream URL is invalid" });
    } catch (const std::exception &failure) {
        return std::unexpected(std::string{ "upstream connect failed: " } + failure.what());
    }
}

void stream_to_client(AttemptContext &ctx, int status, std::string content_type, std::vector<UpstreamHeader> headers,
                      StreamPump pump)
{
    ctx.pending_stream = PendingStream{
        .status = status,
        .content_type = std::move(content_type),
        .headers = std::move(headers),
        .pump = std::move(pump),
    };
}

namespace
{

/*
 * Install a handed-over stream on the response and commit when it ends.
 *
 * The chunked provider is what makes the response head go out before the body
 * exists, which is the whole point of streaming; it also means this runs after
 * dispatch_protocol_request has returned, so the commit has to travel with it.
 */
void install_pending_stream(::httplib::Response &response, PendingStream stream, ProxyRequest proxy)
{
    response.status = stream.status;
    for (const UpstreamHeader &header : stream.headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "connection" || lower == "transfer-encoding" || lower == "content-length" ||
            lower == "content-type") {
            continue;
        }
        response.set_header(header.name, header.value);
    }

    struct Pending {
        PendingStream stream;
        ProxyRequest proxy;
    };
    auto shared = std::make_shared<Pending>(Pending{ std::move(stream), std::move(proxy) });
    const std::string content_type = shared->stream.content_type.empty() ? "text/event-stream; charset=utf-8" :
                                                                           shared->stream.content_type;

    response.set_chunked_content_provider(content_type, [shared](std::size_t offset, ::httplib::DataSink &sink) {
        if (offset != 0) {
            return false;
        }
        // is_writable() before write(): a write to a peer that has gone away
        // succeeds until the kernel buffer fills, so without asking first, a
        // disconnected client looks alive for as long as the upstream keeps
        // talking. The pump uses this answer to switch to its short drain
        // window, so getting it late means holding an upstream connection open
        // for nobody.
        const ClientSink client{
            .write =
                [&sink](std::string_view bytes) {
                    if (!sink.is_writable()) {
                        return false;
                    }
                    return sink.write(bytes.data(), bytes.size());
                },
            .alive = [&sink]() { return sink.is_writable(); },
        };
        try {
            // The pump writes usage into this copy, not into the AttemptContext's
            // ProxyRequest: that one lived on the HTTP handler's stack and is gone
            // by now. This is the object that gets committed below.
            shared->stream.pump(client, shared->proxy);
        } catch (const std::exception &error) {
            // The head is already on the wire, so there is no error response to
            // send. Record what happened and still commit whatever usage the
            // handler had accumulated before it failed.
            shared->proxy.error_message = error.what();
        } catch (...) {
            shared->proxy.error_message = "plugin stream pump raised a non-standard exception";
        }
        commit_proxy_request(shared->proxy);
        sink.done();
        return true;
    });
}

} // namespace

void dispatch_protocol_request(const ::httplib::Request &request, ::httplib::Response &response, ProxyRequest &proxy,
                               ChannelGroup group)
{
    const ProtocolHandler handler = plugin::find_protocol_handler(request.method, request.path, group.type);
    if (handler == nullptr) {
        // No plugin serves this key. The request never enters a plugin, so there
        // is nothing to commit and no record to write.
        write_dispatch_error(response, 500, "no protocol plugin registered for this route");
        return;
    }

    if (group.channels.empty()) {
        // No upstream was ever contacted, so there is nothing to record: a
        // request record means "this reached a channel", and commit_proxy_request
        // enforces that by requiring a channel id.
        write_dispatch_error(response, 503, "no available channel");
        return;
    }

    AttemptContext ctx{
        .channel = nullptr,
        .group_type = group.type,
        .attempt = 0,
        .request = request,
        .response = response,
        .proxy = proxy,
        .pending_stream = std::nullopt,
    };

    for (;;) {
        ctx.channel = &group.channels[static_cast<std::size_t>(group.pointer)];

        AttemptOutcome outcome = AttemptOutcome::Done;
        try {
            outcome = handler(ctx);
        } catch (const std::exception &error) {
            // A plugin that throws gets HTTP 500 and no charge, and the service
            // keeps running. Nothing is committed: we cannot tell how far the
            // handler got, and guessing would either bill for nothing or lose a
            // real charge.
            if (!ctx.pending_stream.has_value()) {
                write_dispatch_error(response, 500, error.what());
            }
            return;
        } catch (...) {
            if (!ctx.pending_stream.has_value()) {
                write_dispatch_error(response, 500, "plugin raised a non-standard exception");
            }
            return;
        }

        if (outcome == AttemptOutcome::Done) {
            break;
        }

        if (ctx.pending_stream.has_value()) {
            // Asking for another candidate after handing over a stream would
            // splice a second upstream's output behind a response head already
            // sent. Treat it as Done and record it, rather than corrupting the
            // stream.
            proxy.error_message = "plugin requested candidate rotation after the response had started";
            break;
        }

        if (ctx.attempt + 1 >= static_cast<int>(group.channels.size())) {
            // One lap and no more: every candidate in the group has had its turn.
            // The rotation is circular, so without this the loop would spin
            // forever whenever every channel is down -- which is exactly when a
            // client is least able to wait. The plugin writes no response when it
            // asks for another candidate, so the last word is the core's.
            write_dispatch_error(response, 502,
                                 proxy.error_message.empty() ? std::string{ "all channels failed" } :
                                                               "all channels failed: " + proxy.error_message);
            break;
        }

        group.next_channel();
        ++ctx.attempt;
        if (const int delay = backoff_delay_ms(ctx.attempt); delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
    }

    // The single exit of the loop, and therefore the single commit. However many
    // candidates were tried, one client request produces one record and one
    // charge -- and a plugin has no way to call this, so it cannot double-bill.
    // A handed-over stream carries the same commit to the end of its pump.
    if (ctx.pending_stream.has_value()) {
        install_pending_stream(response, std::move(*ctx.pending_stream), proxy);
        return;
    }
    commit_proxy_request(proxy);
}

} // namespace revlm
