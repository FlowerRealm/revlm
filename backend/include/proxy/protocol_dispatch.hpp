#pragma once

/*
 * Protocol dispatch and the candidate-rotation loop.
 *
 * This is the whole of what used to be `class Gateway` and its nine virtuals.
 * The core no longer drives a plugin step by step; it looks up one handler,
 * calls it once per upstream attempt, and reads a verdict (ADR 0009).
 */

#include <httplib.h>

#include "channels/channel_groups.hpp"
#include "plugins/data_plane.hpp"
#include "request/proxy_request.hpp"

namespace revlm
{

/*
 * Run one protocol request to completion.
 *
 * Looks the handler up by (method, path, group.type). With no entry, answers
 * HTTP 500 without entering any plugin, without committing and without writing
 * a core request record -- an unrouted request is not a request that happened.
 *
 * Otherwise rotates candidates until a handler returns Done, then commits
 * exactly once. `group` is taken by value on purpose: the rotation pointer it
 * carries is this request's own cursor, not shared state.
 *
 * When the handler handed over a stream, the commit moves to the end of that
 * stream instead. It is still one commit at one exit -- just a later one.
 */
void dispatch_protocol_request(const ::httplib::Request &request, ::httplib::Response &response, ProxyRequest &proxy,
                               ChannelGroup group);

/*
 * Apply the ChannelGroup multiplier to `protocol_cost_usd`, debit the balance and
 * persist the core request record. Called only from the single exit of the
 * rotation loop above, which is what makes double-charging structurally
 * impossible rather than a rule plugin authors have to follow.
 *
 * A request with no usage, or one cut short, is not charged: `protocol_cost_usd`
 * and `usd` stay zero while the captured status, latency and error text are kept.
 */
bool commit_proxy_request(ProxyRequest &proxy);

} // namespace revlm
