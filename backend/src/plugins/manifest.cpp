#include "plugins/manifest.hpp"

#include "plugins/abi.h"
#include "util/json.hpp"

#include <algorithm>
#include <cctype>

namespace revlm::plugin
{
namespace
{

// id and version share one charset: the format doc restricts both to
// letters, digits, '-', '_', '.'. Anything else is rejected outright rather
// than sanitized, because these two fields become a filesystem directory
// name (packages/<id>/) and nothing else in this codebase re-checks that.
bool is_id_charset(std::string_view value)
{
    if (value.empty()) {
        return false;
    }
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.'; });
}

} // namespace

std::expected<PluginManifest, std::string> parse_manifest(std::string_view json_bytes, std::string_view what)
{
    const auto parsed = json::parse(json_bytes);
    if (!parsed.has_value() || !parsed->is_object()) {
        return std::unexpected(std::string{ what } + ": plugin.json is not a JSON object");
    }

    // Strict deconstruction: a manifest missing any of the six keys is a
    // failure, never a silently-defaulted plugin. strict_from reports every
    // missing key in one message.
    auto manifest = strict_from<PluginManifest>(*parsed, what);
    if (!manifest.has_value()) {
        return std::unexpected(std::move(manifest.error()));
    }

    if (!is_id_charset(manifest->id) || !is_id_charset(manifest->version)) {
        return std::unexpected(std::string{ what } +
                               ": id and version may only contain letters, digits, '-', '_', '.'");
    }

    if (manifest->abi_version != REVLM_PLUGIN_ABI_VERSION) {
        return std::unexpected(std::string{ what } + ": manifest abi_version " + std::to_string(manifest->abi_version) +
                               " does not match host control-plane abi_version " +
                               std::to_string(REVLM_PLUGIN_ABI_VERSION));
    }

    return manifest;
}

} // namespace revlm::plugin
