#pragma once

/*
 * Glue between the registry and whichever file drives dlopen and calls each
 * plugin's revlm_plugin_register entry point (ADR 0007). Not part of the
 * public registry surface in plugins/registry.hpp -- that file only exposes
 * what the rest of the core needs to dispatch requests and manage plugin
 * state; this one exists purely so the loader can hand a plugin its table.
 */

#include <httplib.h>

#include <string_view>

#include "plugins/abi.h"

namespace revlm::plugin
{

/*
 * A services table bound to one plugin id, ready to hand to that plugin's
 * register entry point. `server` is the shared httplib::Server that ordinary
 * endpoints (register_endpoint) end up registered on. The embedded host
 * pointer is owned by the registry and lives for the process, so the caller
 * never frees it.
 */
revlm_plugin_services make_plugin_services(std::string_view plugin_id, ::httplib::Server &server);

} // namespace revlm::plugin
