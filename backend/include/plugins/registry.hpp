#pragma once

/*
 * The route table: the core's only map from a request to a plugin.
 *
 * The core pre-populates nothing. Every entry arrives from a plugin's register
 * entry point, which is why the core recognises no protocol name and no path
 * prefix of its own -- a protocol whose URLs look nothing like OpenAI's needs no
 * change here. Lookup is by key, so registration order carries no meaning and
 * plugins need neither a load order nor dependency declarations.
 *
 * Two kinds of key, and the difference is one character. An ordinary path
 * matches itself. A path registered with a trailing '/' also claims everything
 * below it, longest match first, which is how a protocol serves per-item
 * endpoints ("GET /v1/models/") without registering one route per item. The core
 * still parses nothing out of the path: the item id is the plugin's to read off
 * ctx.request.path.
 */

#include <string>
#include <string_view>

#include "plugins/data_plane.hpp"
#include "util/json.hpp"

namespace revlm::plugin
{

/*
 * Look up the one handler for this key. Returns nullptr when no plugin serves
 * it; the core then answers HTTP 500 without entering any plugin, without
 * committing and without writing a core request record.
 */
ProtocolHandler find_protocol_handler(std::string_view method, std::string_view path, std::string_view group_type);

/*
 * Whether any plugin serves this (method, path) under any group type. The
 * catch-all HTTP route asks this before it authenticates, so that a path no
 * plugin claims answers 404 rather than demanding credentials for a route that
 * does not exist.
 */
bool has_route(std::string_view method, std::string_view path);

/* The model catalogue a plugin registered for this group type. A JSON array. */
json models_for_group_type(std::string_view group_type);

/* Every registered model across all group types, as a JSON array. */
json all_models();

/*
 * Drop every entry owned by a plugin. Used when it is disabled: routes and the
 * model catalogue change at once, and requests already in flight are unaffected
 * because nothing loaded is replaced or unloaded.
 *
 * Ordinary global endpoints are the exception. httplib has no way to withdraw a
 * route once registered, so they stay on the server and the core's wrapper
 * answers 404 while their owner is disabled. The observable behaviour matches;
 * only the mechanism differs.
 */
void unregister_plugin(std::string_view plugin_id);

/* Whether a plugin's entries are currently live. */
bool plugin_is_active(std::string_view plugin_id);

} // namespace revlm::plugin
