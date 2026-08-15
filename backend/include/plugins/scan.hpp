#pragma once

/*
 * Enumeration of installed packages on disk. Layout is a single flat level --
 * <plugin_dir>/packages/<id>/ -- with no version directory, no active/
 * symlink and no disabled/ marker; that all belongs to the deleted preload
 * design. Enable/disable state and pending-uninstall state are not filesystem
 * facts (see plugin-package-format.md, "启用状态") and are therefore not this
 * file's concern.
 */

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace revlm::plugin
{

/* "amd" or "arm": the backend/<name>/ directory this host's own architecture
 * resolves to. There is no third value -- an unrecognised build architecture
 * is a compile-time error, not a runtime one (see scan.cpp). */
std::string current_platform_name();

/*
 * Validate one backend/<platform_name>/ directory inside a package: it must
 * exist and contain exactly one regular file with a .so extension. Extra
 * non-.so files may sit alongside it. A missing directory is itself an
 * error for that package, not merely "no module".
 *
 * Returns empty on success, otherwise a message naming what is wrong.
 */
std::string validate_platform_dir(const std::filesystem::path &package_dir, std::string_view platform_name);

/* Validate that frontend/entry.js exists. Content is never inspected -- the
 * format doc allows it to be an empty/no-op ESM module. */
std::string validate_frontend_entry(const std::filesystem::path &package_dir);

/*
 * Validate everything this host needs in order to load package_dir: its own
 * platform module (validate_platform_dir for current_platform_name()) plus
 * the frontend entry point.
 */
std::string validate_package_layout(const std::filesystem::path &package_dir);

struct InstalledPackage {
    std::string id;
    std::filesystem::path dir;
};

/*
 * Every id directory directly under <base>/packages whose name is a valid
 * plugin id. Does not parse plugin.json or validate layout -- a scan just
 * lists what is on disk, it does not judge it.
 */
std::vector<InstalledPackage> installed_packages(const std::filesystem::path &base);

/*
 * installed_packages(config().plugin_dir) merged with
 * installed_packages(config().system_plugin_dir); a user-installed package
 * shadows a system package of the same id.
 */
std::vector<InstalledPackage> all_installed_packages();

} // namespace revlm::plugin
