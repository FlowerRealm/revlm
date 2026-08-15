#pragma once

/*
 * plugin.json parsing. Kept out of host.hpp because nothing outside
 * plugins/manifest.cpp and plugins/install.cpp needs to parse a manifest --
 * everyone else consumes the already-validated PluginManifest.
 */

#include <expected>
#include <string>
#include <string_view>

#include "plugins/host.hpp"

namespace revlm::plugin
{

/*
 * Parse and validate one plugin.json body already read into memory:
 *   - strict_from deconstruction (missing keys reported together, never
 *     defaulted);
 *   - id and version restricted to letters, digits, '-', '_', '.';
 *   - abi_version equal to REVLM_PLUGIN_ABI_VERSION.
 *
 * `what` names the source for error text (a package directory, an archive
 * being installed) so a failure can be traced back to one plugin.
 */
std::expected<PluginManifest, std::string> parse_manifest(std::string_view json_bytes, std::string_view what);

} // namespace revlm::plugin
