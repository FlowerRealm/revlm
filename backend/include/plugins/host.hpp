#pragma once

#include <boost/describe.hpp>

#include <string>
#include <string_view>
#include <vector>

#include "util/json.hpp"

namespace httplib
{
class Server;
}

namespace revlm::plugin
{

/*
 * Inbound: exactly the six keys of plugin.json, nothing else.
 *
 * Runtime facts -- where the package landed, whether it loaded, why it did not --
 * deliberately live in PluginInfo instead. Mixing them in here would make strict
 * deconstruction report them as missing keys on every hand-written manifest, and
 * that confusion is what an eleven-field combined struct cost us last time.
 */
struct PluginManifest {
    std::string id;
    std::string type;
    std::string name;
    std::string description;
    std::string version;
    int abi_version = 0;
};

BOOST_DESCRIBE_STRUCT(PluginManifest, (), (id, type, name, description, version, abi_version))

/* Outbound: one row of the admin plugin list. */
struct PluginInfo {
    std::string id;
    std::string type;
    std::string name;
    std::string description;
    std::string version;
    /* Absolute path of the installed package directory. */
    std::string dir;
    /* "loaded" | "disabled" | "failed" | "pending_uninstall" */
    std::string status;
    /* Empty unless status is "failed". */
    std::string error;
};

BOOST_DESCRIBE_STRUCT(PluginInfo, (), (id, type, name, description, version, dir, status, error))

/*
 * Cold start, in order: run cleanup for packages with a pending uninstall, then
 * for every installed package that is not on the disabled list, run its migration
 * inside its own transaction and dlopen + register it. A plugin that fails either
 * step becomes a failed plugin -- recorded, skipped, and not fatal to the service.
 *
 * Must be called after the core schema is in place and before the server starts
 * accepting traffic. `server` is where ordinary global endpoints land; the
 * reference is retained so that enabling a plugin later can register onto the
 * same server.
 */
void load_plugins(::httplib::Server &server);

/* Current state of every known package, in id order. */
std::vector<PluginInfo> plugin_infos();

/*
 * Install (or replace) a package from an uploaded archive. Validates and unpacks
 * into staging, then atomically swaps it into place. Migrations are not run here:
 * the new package takes effect on the next cold start.
 */
bool install_plugin_archive(std::string_view archive_bytes, std::string &error);

/*
 * Enable or disable a plugin. This takes effect immediately on the registry --
 * routes and the model catalogue change at once, in-flight requests are
 * unaffected -- because it only adds or removes registry entries and never
 * replaces loaded code. Migrations still wait for a cold start, so a plugin
 * enabled for the first time runs its migration then.
 */
bool set_plugin_enabled(std::string_view plugin_id, bool enabled, std::string &error);

/*
 * Mark a plugin for uninstall. The package stays on disk until the next cold
 * start calls its cleanup entry point; only then is it deleted.
 */
bool schedule_plugin_uninstall(std::string_view plugin_id, std::string &error);

} // namespace revlm::plugin
