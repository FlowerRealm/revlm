/*
 * Revlm plugin control-plane ABI.
 *
 * This header is pure C on purpose. Registration, the model catalogue, migration
 * and uninstall cleanup all cross the boundary as function pointers and
 * NUL-terminated JSON, with no C++ standard library or third-party type in
 * sight, so a single integer version number is enough to decide compatibility.
 * The host compares REVLM_PLUGIN_ABI_VERSION against the plugin manifest's
 * abi_version and refuses to load on a mismatch.
 *
 * The data plane -- what a registered protocol handler actually receives -- is
 * C++ and lives in plugins/data_plane.hpp. It is coupled to the host's C++ ABI
 * and upgrades together with it on a cold restart. See ADR 0008 for why the two
 * layers get different stability rules.
 *
 * Nothing here may throw. Failures are return codes plus a message written into
 * a caller-owned buffer; an exception unwinding through this boundary is
 * undefined behaviour.
 */
#ifndef REVLM_PLUGIN_ABI_H
#define REVLM_PLUGIN_ABI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REVLM_PLUGIN_ABI_VERSION 1

/* Size of the error buffer the host passes to every entry point below. */
#define REVLM_PLUGIN_ERROR_SIZE 512

/*
 * A handler pointer in transit. The control plane carries the pointer; the data
 * plane (plugins/data_plane.hpp) defines what it points at. Converting a
 * function pointer to another function pointer type and back is well defined,
 * so the erasure costs nothing but a cast at registration time.
 */
typedef void (*revlm_plugin_fn)(void);

/* The host's registry, opaque to the plugin. */
typedef struct revlm_plugin_host revlm_plugin_host;

/*
 * Handed to the plugin's register entry point. Every call returns 0 on success
 * and non-zero on failure; the host already knows why a registration failed and
 * records it against the plugin, so the plugin only has to propagate.
 */
typedef struct revlm_plugin_services {
    int abi_version;
    revlm_plugin_host *host;

    /*
     * Data-plane route. The key is (method, path, ChannelGroup.type). The path
     * is entirely the plugin's; the host prepends no prefix, so a protocol whose
     * URLs look nothing like OpenAI's needs no host change. Two plugins may
     * serve the same path for different group types. Registering a key that is
     * already taken fails here rather than silently losing to load order.
     *
     * handler is a revlm::ProtocolHandler erased to revlm_plugin_fn.
     */
    int (*register_route)(revlm_plugin_host *host, const char *method, const char *path, const char *group_type,
                          revlm_plugin_fn handler);

    /*
     * Model catalogue for a ChannelGroup.type. models_json is a JSON array; each
     * entry's pricing is the plugin's own shape and the host does not parse it.
     */
    int (*register_models)(revlm_plugin_host *host, const char *group_type, const char *models_json);

    /*
     * An ordinary global endpoint, outside the data plane. Path collisions here
     * fall back to httplib's registration-order semantics and are not detected.
     *
     * handler is a revlm::EndpointHandler erased to revlm_plugin_fn.
     */
    int (*register_endpoint)(revlm_plugin_host *host, const char *method, const char *path, revlm_plugin_fn handler);
} revlm_plugin_services;

/*
 * Entry points a plugin exports. Each returns 0 on success, or non-zero after
 * writing a NUL-terminated message of at most error_size bytes into error.
 *
 * revlm_plugin_register is required and runs once, right after the module is
 * loaded. The other two are optional; a missing symbol is a no-op.
 *
 * revlm_plugin_migrate runs on a cold start, for enabled plugins only, after the
 * core schema. The host wraps each call in its own transaction, so a failure
 * rolls back only that plugin and leaves the rest of the service running.
 *
 * revlm_plugin_cleanup runs on a cold start for plugins with a pending uninstall,
 * before the service accepts traffic.
 */
typedef int (*revlm_plugin_register_fn)(const revlm_plugin_services *services, char *error, size_t error_size);
typedef int (*revlm_plugin_migrate_fn)(char *error, size_t error_size);
typedef int (*revlm_plugin_cleanup_fn)(char *error, size_t error_size);

#define REVLM_PLUGIN_REGISTER_SYMBOL "revlm_plugin_register"
#define REVLM_PLUGIN_MIGRATE_SYMBOL "revlm_plugin_migrate"
#define REVLM_PLUGIN_CLEANUP_SYMBOL "revlm_plugin_cleanup"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* REVLM_PLUGIN_ABI_H */
