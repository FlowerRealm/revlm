#include "plugins/packages.hpp"

#include "config/config.hpp"
#include "util/strings.hpp"

#include <zlib.h>

#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace revlm::plugin
{
namespace
{

namespace fs = std::filesystem;

constexpr std::size_t k_hard_max_archive_bytes = 512U * 1024U * 1024U;
constexpr std::size_t k_max_file_bytes = 128U * 1024U * 1024U;
constexpr std::size_t k_max_unpacked_bytes = 512U * 1024U * 1024U;

constexpr std::string_view k_migrate_symbol = "revlm_plugin_migrate";
constexpr std::string_view k_cleanup_symbol = "revlm_plugin_cleanup";

using LifecycleFn = void (*)();

std::string plugin_file(const fs::path &path, std::size_t maximum = k_max_file_bytes)
{
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > maximum) {
        throw std::runtime_error("plugin file is missing or too large: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to read plugin file: " + path.string());
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input && !input.eof()) {
        throw std::runtime_error("unable to read plugin file: " + path.string());
    }
    return bytes;
}

void write_plugin_file(const fs::path &path, std::string_view bytes)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        throw std::runtime_error("unable to create plugin directory: " + path.parent_path().string());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("unable to write plugin file: " + path.string());
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("unable to finish plugin file: " + path.string());
    }
}

// An archive or frontend path must be a relative POSIX path with no `..`,
// absolute components, backslashes or NUL bytes.
bool safe_archive_path(std::string_view raw)
{
    if (raw.empty() || raw.front() == '/' || raw.find('\\') != std::string_view::npos ||
        raw.find('\0') != std::string_view::npos) {
        return false;
    }
    const fs::path path{ raw };
    if (path.is_absolute()) {
        return false;
    }
    for (const fs::path &part : path) {
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
    }
    return true;
}

bool path_is_within(const fs::path &root, const fs::path &candidate)
{
    std::error_code error;
    const fs::path normalized_root = fs::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const fs::path normalized_candidate = fs::weakly_canonical(candidate, error);
    if (error) {
        return false;
    }
    auto root_it = normalized_root.begin();
    auto candidate_it = normalized_candidate.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == normalized_candidate.end() || *root_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

std::string random_suffix()
{
    std::random_device device;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(static_cast<unsigned long long>(now)) + "-" + std::to_string(::getpid()) + "-" +
           std::to_string(device());
}

fs::path user_plugin_root()
{
    return fs::path{ config().plugin_dir };
}

fs::path system_plugin_root()
{
    return fs::path{ config().system_plugin_dir };
}

struct ZipEntry {
    std::string path;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t local_header_offset = 0;
    std::uint32_t external_attributes = 0;
};

std::uint16_t zip_u16(std::string_view bytes, std::size_t offset)
{
    if (offset + 2 > bytes.size()) {
        throw std::runtime_error("truncated plugin archive");
    }
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset])) |
           static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset + 1]) << 8U);
}

std::uint32_t zip_u32(std::string_view bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("truncated plugin archive");
    }
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1]) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2]) << 16U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3]) << 24U);
}

std::string inflate_raw(std::string_view input, std::size_t expected_size)
{
    if (expected_size > k_max_file_bytes) {
        throw std::runtime_error("plugin archive entry is too large");
    }
    std::string output(std::max<std::size_t>(expected_size, 1), '\0');
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef *>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    if (::inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("unable to initialize plugin archive decompressor");
    }
    const int result = ::inflate(&stream, Z_FINISH);
    (void)::inflateEnd(&stream);
    if (result != Z_STREAM_END || stream.total_out != expected_size) {
        throw std::runtime_error("invalid deflate stream in plugin archive");
    }
    output.resize(expected_size);
    return output;
}

void extract_plugin_archive(std::string_view archive, const fs::path &destination)
{
    if (archive.size() < 22 || archive.size() > static_cast<std::size_t>(config().plugin_max_archive_bytes) ||
        archive.size() > k_hard_max_archive_bytes) {
        throw std::runtime_error("plugin archive is empty or too large");
    }
    const std::size_t search_start = archive.size() > 65535U + 22U ? archive.size() - (65535U + 22U) : 0;
    std::optional<std::size_t> eocd;
    for (std::size_t position = archive.size() - 22;; --position) {
        if (zip_u32(archive, position) == 0x06054b50U) {
            eocd = position;
            break;
        }
        if (position == search_start) {
            break;
        }
    }
    if (!eocd.has_value()) {
        throw std::runtime_error("plugin archive is not a supported ZIP file");
    }
    const std::uint16_t disk = zip_u16(archive, *eocd + 4);
    const std::uint16_t central_disk = zip_u16(archive, *eocd + 6);
    const std::uint16_t entries_on_disk = zip_u16(archive, *eocd + 8);
    const std::uint16_t entries = zip_u16(archive, *eocd + 10);
    const std::uint32_t central_size = zip_u32(archive, *eocd + 12);
    const std::uint32_t central_offset = zip_u32(archive, *eocd + 16);
    if (disk != 0 || central_disk != 0 || entries_on_disk != entries || entries == 0xffffU ||
        central_offset == 0xffffffffU || central_size == 0xffffffffU ||
        static_cast<std::size_t>(central_offset) + central_size > archive.size()) {
        throw std::runtime_error("unsupported multi-volume or ZIP64 plugin archive");
    }

    std::vector<ZipEntry> entries_out;
    entries_out.reserve(entries);
    std::unordered_set<std::string> paths;
    std::size_t cursor = central_offset;
    std::size_t total_unpacked = 0;
    for (std::uint16_t index = 0; index < entries; ++index) {
        if (zip_u32(archive, cursor) != 0x02014b50U) {
            throw std::runtime_error("invalid plugin ZIP central directory");
        }
        const std::uint16_t flags = zip_u16(archive, cursor + 8);
        const std::uint16_t method = zip_u16(archive, cursor + 10);
        const std::uint32_t crc = zip_u32(archive, cursor + 16);
        const std::uint32_t compressed_size = zip_u32(archive, cursor + 20);
        const std::uint32_t uncompressed_size = zip_u32(archive, cursor + 24);
        const std::uint16_t name_size = zip_u16(archive, cursor + 28);
        const std::uint16_t extra_size = zip_u16(archive, cursor + 30);
        const std::uint16_t comment_size = zip_u16(archive, cursor + 32);
        const std::uint32_t attributes = zip_u32(archive, cursor + 38);
        const std::uint32_t local_offset = zip_u32(archive, cursor + 42);
        const std::size_t entry_end = cursor + 46U + name_size + extra_size + comment_size;
        if (entry_end > archive.size()) {
            throw std::runtime_error("truncated plugin ZIP entry");
        }
        std::string path{ archive.substr(cursor + 46U, name_size) };
        cursor = entry_end;
        if (!safe_archive_path(path) || (flags & 0x1U) != 0U || (method != 0U && method != 8U) ||
            ((attributes >> 16U) & S_IFMT) == S_IFLNK || uncompressed_size > k_max_file_bytes ||
            compressed_size > k_hard_max_archive_bytes || total_unpacked + uncompressed_size > k_max_unpacked_bytes ||
            !paths.emplace(path).second) {
            throw std::runtime_error("invalid plugin archive entry");
        }
        total_unpacked += uncompressed_size;
        entries_out.push_back(
            { std::move(path), flags, method, crc, compressed_size, uncompressed_size, local_offset, attributes });
    }

    std::error_code error;
    fs::create_directories(destination, error);
    if (error) {
        throw std::runtime_error("unable to create plugin staging directory");
    }
    for (const ZipEntry &entry : entries_out) {
        const fs::path output = destination / entry.path;
        if (entry.path.back() == '/') {
            fs::create_directories(output, error);
            if (error) {
                throw std::runtime_error("unable to create plugin archive directory");
            }
            continue;
        }
        if (static_cast<std::size_t>(entry.local_header_offset) + 30U > archive.size() ||
            zip_u32(archive, entry.local_header_offset) != 0x04034b50U) {
            throw std::runtime_error("invalid plugin ZIP local entry");
        }
        const std::uint16_t local_name_size = zip_u16(archive, entry.local_header_offset + 26);
        const std::uint16_t local_extra_size = zip_u16(archive, entry.local_header_offset + 28);
        const std::size_t data_offset =
            static_cast<std::size_t>(entry.local_header_offset) + 30U + local_name_size + local_extra_size;
        if (data_offset + entry.compressed_size > archive.size()) {
            throw std::runtime_error("truncated plugin ZIP payload");
        }
        const std::string_view compressed = archive.substr(data_offset, entry.compressed_size);
        const std::string bytes = entry.method == 0U ? std::string{ compressed } :
                                                       inflate_raw(compressed, entry.uncompressed_size);
        if (bytes.size() != entry.uncompressed_size ||
            static_cast<std::uint32_t>(::crc32(0, reinterpret_cast<const Bytef *>(bytes.data()),
                                               static_cast<uInt>(bytes.size()))) != entry.crc) {
            throw std::runtime_error("plugin archive CRC validation failed");
        }
        write_plugin_file(output, bytes);
    }
}

// -- filesystem enable-state markers -----------------------------------------
//
// State is expressed entirely as marker files under REVLM_PLUGIN_DIR:
//   active/<id>    positive record written on install/enable
//   disabled/<id>  negative marker written on disable
//   pending/<id>   pending uninstall marker
//   failed/<id>    carries the cleanup error message after a failed uninstall
// The negative markers are authoritative: a plugin is enabled unless it has a
// disabled or pending marker. System packages carry no markers and therefore
// default to enabled.

fs::path marker_path(const fs::path &plugin_dir, std::string_view dir, std::string_view id)
{
    return plugin_dir / std::string{ dir } / std::string{ id };
}

bool has_marker(const fs::path &plugin_dir, std::string_view dir, std::string_view id)
{
    std::error_code error;
    return fs::is_regular_file(marker_path(plugin_dir, dir, id), error);
}

bool plugin_is_enabled(const fs::path &plugin_dir, std::string_view id)
{
    return !has_marker(plugin_dir, "disabled", id) && !has_marker(plugin_dir, "pending", id);
}

void write_marker(const fs::path &plugin_dir, std::string_view dir, std::string_view id, std::string_view content)
{
    write_plugin_file(marker_path(plugin_dir, dir, id), content);
}

void remove_marker(const fs::path &plugin_dir, std::string_view dir, std::string_view id)
{
    std::error_code error;
    fs::remove(marker_path(plugin_dir, dir, id), error);
}

void mark_enabled(const fs::path &plugin_dir, std::string_view id)
{
    write_marker(plugin_dir, "active", id, "enabled\n");
    remove_marker(plugin_dir, "disabled", id);
    remove_marker(plugin_dir, "pending", id);
    remove_marker(plugin_dir, "failed", id);
}

void mark_disabled(const fs::path &plugin_dir, std::string_view id)
{
    write_marker(plugin_dir, "disabled", id, "disabled\n");
    remove_marker(plugin_dir, "active", id);
}

// -- lifecycle symbols -------------------------------------------------------

void call_migrate(const fs::path &module)
{
    void *handle = ::dlopen(module.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (handle == nullptr) {
        // The module must load: it is about to be put into LD_PRELOAD. A module
        // that cannot even be loaded here would break the worker too.
        const char *message = ::dlerror();
        throw std::runtime_error("unable to load plugin module " + module.string() + ": " +
                                 (message == nullptr ? std::string{ "unknown error" } : std::string{ message }));
    }
    void *symbol = ::dlsym(handle, k_migrate_symbol.data());
    if (symbol == nullptr) {
        return; // symbol missing is a no-op
    }
    reinterpret_cast<LifecycleFn>(symbol)(); // a thrown C++ exception is a failure
}

void call_cleanup(const fs::path &module)
{
    void *handle = ::dlopen(module.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (handle == nullptr) {
        return; // unloadable module: nothing to clean, do not block removal
    }
    void *symbol = ::dlsym(handle, k_cleanup_symbol.data());
    if (symbol == nullptr) {
        return; // symbol missing is a no-op
    }
    reinterpret_cast<LifecycleFn>(symbol)(); // a thrown C++ exception is a failure
}

// -- pending uninstalls and update backups -----------------------------------

fs::path backup_root(const fs::path &plugin_dir)
{
    return plugin_dir / "backups";
}

fs::path backup_package(const fs::path &plugin_dir, std::string_view id)
{
    return backup_root(plugin_dir) / std::string{ id };
}

fs::path backup_state_marker(const fs::path &plugin_dir, std::string_view id)
{
    return backup_root(plugin_dir) / (std::string{ id } + ".enabled");
}

void discard_backup(const fs::path &plugin_dir, std::string_view id)
{
    std::error_code error;
    fs::remove_all(backup_package(plugin_dir, id), error);
    fs::remove(backup_state_marker(plugin_dir, id), error);
}

void restore_backup(const fs::path &plugin_dir, std::string_view id)
{
    std::error_code error;
    const fs::path package = plugin_dir / "packages" / std::string{ id };
    if (!fs::is_directory(backup_package(plugin_dir, id), error)) {
        return;
    }
    fs::remove_all(package, error);
    fs::rename(backup_package(plugin_dir, id), package, error);
    if (error) {
        throw std::runtime_error("unable to restore previous plugin package for " + std::string{ id });
    }
    // Restore the enable state recorded before the update landed.
    std::error_code ignored;
    if (fs::is_regular_file(backup_state_marker(plugin_dir, id), ignored)) {
        const std::string state = trim_ascii(plugin_file(backup_state_marker(plugin_dir, id)));
        if (state == "disabled") {
            mark_disabled(plugin_dir, id);
        } else {
            mark_enabled(plugin_dir, id);
        }
    }
    fs::remove(backup_state_marker(plugin_dir, id), ignored);
}

// Runs cleanup for every pending uninstall, then deletes the package. A thrown
// cleanup exception keeps the package, marks it failed and does not retry until
// the user explicitly uninstalls again. A package whose module cannot load has
// its cleanup treated as a no-op: the user wants it gone.
void process_pending_uninstalls(const fs::path &plugin_dir)
{
    std::error_code error;
    const fs::path pending_dir = plugin_dir / "pending";
    if (!fs::is_directory(pending_dir, error)) {
        return;
    }
    for (const fs::directory_entry &entry : fs::directory_iterator(pending_dir, error)) {
        if (error) {
            break;
        }
        const std::string id = entry.path().filename().string();
        if (!plugin_identifier_is_safe(id) || !entry.is_regular_file(error)) {
            continue;
        }
        const fs::path package = plugin_dir / "packages" / id;
        std::string failure;
        if (fs::is_directory(package, error)) {
            try {
                // The module path comes from the directory convention, not the
                // manifest: a pending uninstall must still clean up and go away
                // even if plugin.json was corrupted after scheduling.
                const auto module_rel = module_path_for_platform(package, current_plugin_platform());
                std::error_code module_error;
                if (module_rel.has_value() && fs::is_regular_file(package / *module_rel, module_error)) {
                    call_cleanup(package / *module_rel);
                }
            } catch (const std::exception &cleanup_error) {
                failure = trim_ascii(cleanup_error.what());
            } catch (...) {
                failure = "unknown cleanup exception";
            }
        }
        if (!failure.empty()) {
            // Keep the package and mark it failed; drop the pending marker so
            // cleanup is not retried automatically. An explicit re-uninstall
            // re-schedules it. The disabled marker stays: a failed-cleanup
            // plugin is not loaded.
            remove_marker(plugin_dir, "pending", id);
            write_marker(plugin_dir, "failed", id, failure + "\n");
            continue;
        }
        fs::remove_all(package, error);
        remove_marker(plugin_dir, "pending", id);
        remove_marker(plugin_dir, "active", id);
        remove_marker(plugin_dir, "disabled", id);
        remove_marker(plugin_dir, "failed", id);
        discard_backup(plugin_dir, id);
    }
}

// The bootstrap frontend snapshot. The worker reads the exact package set that
// the bootstrap recorded when it built LD_PRELOAD, so an upload or enable
// change never mutates an already-running worker's asset list.
struct WorkerPluginRoot {
    std::string id;
    fs::path root;
};

std::vector<WorkerPluginRoot> worker_plugin_roots()
{
    const char *raw = std::getenv("REVLM_PRELOADED_PLUGIN_ROOTS");
    if (raw == nullptr || *raw == '\0') {
        return {};
    }
    std::vector<WorkerPluginRoot> out;
    std::string_view remaining{ raw };
    while (!remaining.empty()) {
        const std::size_t newline = remaining.find('\n');
        const std::string_view row = remaining.substr(0, newline);
        const std::size_t tab = row.find('\t');
        if (tab != std::string_view::npos) {
            const std::string id{ row.substr(0, tab) };
            const std::string path{ row.substr(tab + 1) };
            if (plugin_identifier_is_safe(id) && !path.empty()) {
                out.push_back({ std::move(id), fs::path{ path } });
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(newline + 1);
    }
    return out;
}

} // namespace

PluginActionResult install_plugin_archive(std::string_view archive_bytes)
{
    fs::path staging;
    bool replaced = false;
    std::string id;
    const fs::path root = user_plugin_root();
    try {
        std::error_code error;
        fs::create_directories(root / "staging", error);
        if (error) {
            throw std::runtime_error("unable to create plugin storage");
        }
        staging = root / "staging" / ("upload-" + random_suffix());
        extract_plugin_archive(archive_bytes, staging);

        const PluginPackage package = read_plugin_package(staging);
        id = package.id;
        const PluginPlatform platform = current_plugin_platform();

        // A production package must carry both platform dirs, each with exactly
        // one loadable .so; the current platform must be among them.
        const auto amd = module_path_for_platform(staging, PluginPlatform{ platform.os, "amd" });
        const auto arm = module_path_for_platform(staging, PluginPlatform{ platform.os, "arm" });
        if (!amd.has_value() || !arm.has_value()) {
            throw std::runtime_error(
                "plugin package must contain backend/amd and backend/arm, each with exactly one .so");
        }
        std::error_code module_error;
        if (!fs::is_regular_file(staging / *module_path_for_platform(staging, platform), module_error)) {
            throw std::runtime_error("plugin package has no module for this host platform");
        }

        const fs::path destination = root / "packages" / id;
        fs::create_directories(destination.parent_path(), error);
        if (error) {
            throw std::runtime_error("unable to create plugin package directory");
        }
        const bool is_update = fs::exists(destination, error);
        // An upload supersedes a previously failed cleanup or a scheduled
        // uninstall for this id, but it never silently flips a disabled plugin
        // back on: enable state is a property of the id, not of the package.
        remove_marker(root, "pending", id);
        remove_marker(root, "failed", id);
        if (is_update) {
            // Replace in place. Snapshot the old enable state, move the old
            // package aside as a temporary backup, then publish the new package.
            // The backup survives until the next cold-start migration finishes.
            const std::string old_state = plugin_is_enabled(root, id) ? "enabled" : "disabled";
            discard_backup(root, id);
            fs::create_directories(backup_root(root), error);
            if (error) {
                throw std::runtime_error("unable to create plugin backup directory");
            }
            fs::rename(destination, backup_package(root, id), error);
            if (error) {
                throw std::runtime_error("unable to back up previous plugin package");
            }
            write_plugin_file(backup_state_marker(root, id), old_state + "\n");
            replaced = true;
        }

        fs::rename(staging, destination, error);
        if (error) {
            throw std::runtime_error("unable to atomically install plugin package");
        }
        staging.clear();
        if (!replaced) {
            // Fresh install: an explicit active marker records the enabled state.
            mark_enabled(root, id);
        }
        return { true, "安装完成；下次冷启动执行迁移并以预加载模块方式运行" };
    } catch (const std::exception &install_error) {
        std::error_code ignored;
        if (!staging.empty()) {
            fs::remove_all(staging, ignored);
        }
        if (replaced && !id.empty()) {
            // Publish failed after the old package was moved aside; put it back.
            const fs::path package = root / "packages" / id;
            if (!fs::is_directory(package, ignored)) {
                fs::rename(backup_package(root, id), package, ignored);
            }
            fs::remove(backup_state_marker(root, id), ignored);
        }
        return { false, trim_ascii(install_error.what()) };
    } catch (...) {
        std::error_code ignored;
        if (!staging.empty()) {
            fs::remove_all(staging, ignored);
        }
        return { false, "unknown install error" };
    }
}

PluginActionResult set_plugin_enabled(std::string_view raw_id, bool enabled)
{
    const std::string id = trim_ascii(raw_id);
    if (!plugin_identifier_is_safe(id)) {
        return { false, "插件 ID 无效" };
    }
    try {
        const fs::path root = user_plugin_root();
        std::error_code error;
        if (!fs::is_directory(root / "packages" / id, error)) {
            return { false, "插件不存在" };
        }
        if (enabled) {
            mark_enabled(root, id);
            return { true, "已启用；下次冷启动执行迁移并生效" };
        }
        mark_disabled(root, id);
        return { true, "已停用；下次冷启动不再加载" };
    } catch (const std::exception &error) {
        return { false, trim_ascii(error.what()) };
    }
}

PluginActionResult schedule_plugin_uninstall(std::string_view raw_id)
{
    const std::string id = trim_ascii(raw_id);
    if (!plugin_identifier_is_safe(id)) {
        return { false, "插件 ID 无效" };
    }
    try {
        const fs::path root = user_plugin_root();
        std::error_code error;
        const bool user_package = fs::is_directory(root / "packages" / id, error);
        if (!user_package) {
            if (fs::is_directory(system_plugin_root() / "packages" / id, error)) {
                return { false, "镜像内插件只能停用，不能卸载" };
            }
            return { false, "插件不存在" };
        }
        // Clearing the failed marker lets a previously failed cleanup be
        // retried on the next cold start (the pending marker is already set).
        remove_marker(root, "failed", id);
        write_marker(root, "pending", id, "pending\n");
        mark_disabled(root, id);
        return { true, "已安排卸载；下次冷启动执行清理后删除包，业务数据由插件自行处理" };
    } catch (const std::exception &error) {
        return { false, trim_ascii(error.what()) };
    }
}

std::vector<ActivePlugin> prepare_plugins_for_worker()
{
    std::error_code error;
    const fs::path root = user_plugin_root();
    for (const char *dir : { "packages", "active", "disabled", "pending", "failed", "backups", "staging" }) {
        fs::create_directories(root / dir, error);
        if (error) {
            throw std::runtime_error("unable to create plugin state directory");
        }
    }

    // Uninstall cleanup runs before anything else: the uninstalled module must
    // still be loadable here, and it must not be migrated again.
    process_pending_uninstalls(root);

    const std::vector<ActivePlugin> active = active_plugins(root, system_plugin_root());

    // Migrate every enabled plugin in id-lexicographic order. A failure refuses
    // to start the worker; if this package was an update, the pre-update package
    // and its enable state are restored first.
    for (const ActivePlugin &plugin : active) {
        try {
            call_migrate(plugin.module);
        } catch (const std::exception &migrate_error) {
            try {
                restore_backup(root, plugin.package.id);
            } catch (const std::exception &restore_error) {
                throw std::runtime_error("plugin migration failed for " + plugin.package.id + ": " +
                                         trim_ascii(migrate_error.what()) +
                                         " (and restoring the previous package "
                                         "failed: " +
                                         trim_ascii(restore_error.what()) + ")");
            }
            throw std::runtime_error("plugin migration failed for " + plugin.package.id + ": " +
                                     trim_ascii(migrate_error.what()));
        }
    }

    // Every migration above succeeded, so every backup is disposable: an
    // update backup only exists to roll back a failed migration of the same id.
    // Discard them all here so the directory cannot grow without bound.
    {
        std::error_code ignored;
        if (fs::is_directory(backup_root(root), ignored)) {
            for (const fs::directory_entry &entry : fs::directory_iterator(backup_root(root), ignored)) {
                if (ignored) {
                    break;
                }
                fs::remove_all(entry.path(), ignored);
            }
        }
    }
    return active;
}

json plugin_frontend_entries_json()
{
    json out = json::array();
    for (const WorkerPluginRoot &active : worker_plugin_roots()) {
        const fs::path entry = active.root / "frontend" / "entry.js";
        std::error_code error;
        if (fs::is_regular_file(entry, error)) {
            out.push_back(json({ { "id", active.id }, { "url", "/api/plugins/frontend/" + active.id + "/entry.js" } }));
        }
    }
    return out;
}

std::optional<fs::path> plugin_frontend_file(std::string_view raw_id, std::string_view raw_relative_path)
{
    const std::string id = trim_ascii(raw_id);
    const std::string relative_path = trim_ascii(raw_relative_path);
    if (!plugin_identifier_is_safe(id) || !safe_archive_path(relative_path)) {
        return std::nullopt;
    }
    for (const WorkerPluginRoot &active : worker_plugin_roots()) {
        if (active.id != id) {
            continue;
        }
        const fs::path frontend_root = active.root / "frontend";
        const fs::path candidate = frontend_root / relative_path;
        std::error_code error;
        if (fs::is_regular_file(candidate, error) && path_is_within(frontend_root, candidate)) {
            return candidate;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace revlm::plugin
