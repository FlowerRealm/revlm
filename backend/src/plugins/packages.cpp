#include "plugins/packages.hpp"

#include "config/config.hpp"
#include "store/database.hpp"
#include "util/strings.hpp"

#include <zlib.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace revlm::plugin
{
namespace
{

namespace fs = std::filesystem;

constexpr std::size_t k_hard_max_archive_bytes = 512U * 1024U * 1024U;
constexpr std::size_t k_max_file_bytes = 128U * 1024U * 1024U;
constexpr std::size_t k_max_unpacked_bytes = 512U * 1024U * 1024U;

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

std::vector<PluginInstallation> installations()
{
    const auto rows = sql_query_rows(database(),
                                     "SELECT plugin_id,version,display_name,core_abi,status,package_path,target_os,"
                                     "target_arch,error_message,enabled,system_plugin "
                                     "FROM plugin_installations ORDER BY plugin_id");
    std::vector<PluginInstallation> out;
    out.reserve(rows.size());
    for (const SqlResultRow &row : rows) {
        if (row.size() == 11) {
            out.push_back({ row[0].value_or(""), row[1].value_or(""), row[2].value_or(""), row[3].value_or(""),
                            row[4].value_or(""), row[5].value_or(""), row[6].value_or(""), row[7].value_or(""),
                            row[8].value_or(""), row[9].value_or("0") == "1", row[10].value_or("0") == "1" });
        }
    }
    return out;
}

void save_installation(const PluginInstallation &installation)
{
    odb::database &db = database();
    const auto quote = [&](std::string_view value) { return sql_quote(db, value); };
    sql_exec(db, "INSERT INTO plugin_installations "
                 "(plugin_id,version,display_name,core_abi,status,package_path,target_os,target_arch,error_message,"
                 "enabled,system_plugin) VALUES (" +
                     quote(installation.id) + "," + quote(installation.version) + "," +
                     quote(installation.display_name) + "," + quote(installation.core_abi) + "," +
                     quote(installation.status) + "," + quote(installation.package_path) + "," +
                     quote(installation.target_os) + "," + quote(installation.target_arch) + "," +
                     quote(installation.error_message) + "," + (installation.enabled ? "1" : "0") + "," +
                     (installation.system_plugin ? "1" : "0") +
                     ") ON DUPLICATE KEY UPDATE version=VALUES(version),display_name=VALUES(display_name),"
                     "core_abi=VALUES(core_abi),status=VALUES(status),package_path=VALUES(package_path),"
                     "target_os=VALUES(target_os),target_arch=VALUES(target_arch),error_message=VALUES(error_message),"
                     "enabled=VALUES(enabled),system_plugin=VALUES(system_plugin)");
}

void set_installation_state(std::string_view id, std::string_view status, std::string_view message,
                            std::optional<bool> enabled = std::nullopt)
{
    odb::database &db = database();
    std::string statement =
        "UPDATE plugin_installations SET status=" + sql_quote(db, status) + ",error_message=" + sql_quote(db, message);
    if (enabled.has_value()) {
        statement += ",enabled=";
        statement += *enabled ? "1" : "0";
    }
    statement += " WHERE plugin_id=" + sql_quote(db, id);
    sql_exec(db, statement);
}

void replace_active_link(const std::string &id, const fs::path &target)
{
    const fs::path active = user_plugin_root() / "active";
    std::error_code error;
    fs::create_directories(active, error);
    if (error) {
        throw std::runtime_error("unable to create active plugin directory");
    }
    const fs::path temporary = active / ("." + id + "." + random_suffix());
    fs::create_directory_symlink(target, temporary, error);
    if (error) {
        throw std::runtime_error("unable to activate plugin package");
    }
    fs::rename(temporary, active / id, error);
    if (error) {
        fs::remove(temporary, error);
        throw std::runtime_error("unable to publish active plugin package");
    }
}

void remove_active_link(const std::string &id)
{
    std::error_code error;
    fs::remove(user_plugin_root() / "active" / id, error);
    if (error) {
        throw std::runtime_error("unable to change active plugin package");
    }
}

void set_disabled_marker(const std::string &id, bool disabled)
{
    const fs::path marker = user_plugin_root() / "disabled" / id;
    std::error_code error;
    if (!disabled) {
        fs::remove(marker, error);
        if (error) {
            throw std::runtime_error("unable to enable plugin package");
        }
        return;
    }
    fs::create_directories(marker.parent_path(), error);
    if (error) {
        throw std::runtime_error("unable to create plugin state directory");
    }
    write_plugin_file(marker, "disabled\n");
}

std::vector<std::pair<std::string, std::string>> migration_sql(const ActivePlugin &active)
{
    std::vector<std::pair<std::string, std::string>> migrations;
    migrations.reserve(active.package.migrations.size());
    for (const std::string &path : active.package.migrations) {
        migrations.emplace_back(path, plugin_file(active.root / path));
    }
    return migrations;
}

void apply_migrations(const ActivePlugin &active)
{
    const auto migrations = migration_sql(active);
    if (migrations.empty()) {
        return;
    }
    odb::database &db = database();
    ScopedTransaction transaction(db);
    if (sql_query_one(db, "SELECT GET_LOCK('revlm_plugin_migrations', 30)").value_or("0") != "1") {
        throw std::runtime_error("unable to acquire plugin migration lock");
    }
    try {
        for (const auto &[id, sql] : migrations) {
            const std::string exists =
                "SELECT 1 FROM plugin_migrations WHERE plugin_id=" + sql_quote(db, active.package.id) +
                " AND migration_id=" + sql_quote(db, id) + " LIMIT 1";
            if (sql_query_one(db, exists).value_or("") == "1") {
                continue;
            }
            sql_exec(db, sql);
            sql_exec(db, "INSERT INTO plugin_migrations (plugin_id,migration_id) VALUES (" +
                             sql_quote(db, active.package.id) + "," + sql_quote(db, id) + ")");
        }
        (void)sql_query_one(db, "SELECT RELEASE_LOCK('revlm_plugin_migrations')");
        transaction.commit();
    } catch (...) {
        try {
            (void)sql_query_one(db, "SELECT RELEASE_LOCK('revlm_plugin_migrations')");
        } catch (...) {
        }
        throw;
    }
}

void discover_system_packages()
{
    const PluginPlatform platform = current_plugin_platform();
    std::unordered_map<std::string, PluginInstallation> rows;
    for (const PluginInstallation &installation : installations()) {
        rows.emplace(installation.id, installation);
    }
    const fs::path package_dir = system_plugin_root() / "packages";
    std::error_code error;
    if (!fs::is_directory(package_dir, error)) {
        return;
    }
    for (const fs::directory_entry &id_entry : fs::directory_iterator(package_dir, error)) {
        if (error || !id_entry.is_directory(error)) {
            continue;
        }
        std::vector<fs::path> versions;
        for (const fs::directory_entry &version : fs::directory_iterator(id_entry.path(), error)) {
            if (!error && version.is_directory(error)) {
                versions.push_back(version.path());
            }
        }
        std::sort(versions.begin(), versions.end());
        if (versions.empty()) {
            continue;
        }
        try {
            const fs::path root = versions.back();
            const PluginPackage package = read_plugin_package(root);
            const PluginModule *module = module_for_platform(package, platform);
            if (module == nullptr || !fs::is_regular_file(root / module->path)) {
                throw std::runtime_error("no compatible module");
            }
            const auto existing = rows.find(package.id);
            if (existing != rows.end() && !existing->second.system_plugin) {
                continue;
            }
            const bool enabled = existing == rows.end() ? true : existing->second.enabled;
            save_installation({ package.id, package.version, package.name, package.sdk_abi,
                                enabled ? "pending_restart" : "disabled", root.string(), platform.os, platform.arch, "",
                                enabled, true });
        } catch (const std::exception &) {
            // Bad image contents are a release problem, not a reason to take
            // the control plane down.
        }
    }
}

void remove_pending_uninstalls()
{
    for (const PluginInstallation &installation : installations()) {
        if (installation.status != "pending_uninstall") {
            continue;
        }
        try {
            if (!installation.system_plugin) {
                const fs::path root{ installation.package_path };
                if (!path_is_within(user_plugin_root() / "packages", root)) {
                    throw std::runtime_error("plugin package path is outside REVLM_PLUGIN_DIR");
                }
                std::error_code error;
                fs::remove_all(root, error);
                if (error) {
                    throw std::runtime_error("unable to remove plugin package");
                }
            }
            sql_exec(database(),
                     "DELETE FROM plugin_installations WHERE plugin_id=" + sql_quote(database(), installation.id));
        } catch (const std::exception &error) {
            set_installation_state(installation.id, "failed", trim_ascii(error.what()), false);
        }
    }
}

bool is_selected(const std::vector<ActivePlugin> &active, std::string_view id)
{
    return std::any_of(active.begin(), active.end(), [&](const ActivePlugin &item) { return item.package.id == id; });
}

} // namespace

PluginActionResult install_plugin_archive(std::string_view archive_bytes)
{
    fs::path staging;
    fs::path installed;
    try {
        const fs::path root = user_plugin_root();
        std::error_code error;
        fs::create_directories(root / "staging", error);
        if (error) {
            throw std::runtime_error("unable to create plugin storage");
        }
        staging = root / "staging" / ("upload-" + random_suffix());
        extract_plugin_archive(archive_bytes, staging);
        const PluginPackage package = read_plugin_package(staging);
        const PluginPlatform platform = current_plugin_platform();
        const PluginModule *module = module_for_platform(package, platform);
        if (module == nullptr || !fs::is_regular_file(staging / module->path)) {
            throw std::runtime_error("plugin package has no module for " + platform.os + "/" + platform.arch);
        }
        const fs::path destination = root / "packages" / package.id / package.version;
        fs::create_directories(destination.parent_path(), error);
        if (error || fs::exists(destination)) {
            throw std::runtime_error(error ? "unable to create plugin package directory" :
                                             "plugin version is already installed");
        }
        fs::rename(staging, destination, error);
        if (error) {
            throw std::runtime_error("unable to atomically install plugin package");
        }
        staging.clear();
        installed = destination;
        replace_active_link(package.id, destination);
        set_disabled_marker(package.id, false);
        save_installation({ package.id, package.version, package.name, package.sdk_abi, "pending_restart",
                            destination.string(), platform.os, platform.arch, "", true, false });
        installed.clear();
        return { true, "安装完成；下次重启会加载 v1 插件模块" };
    } catch (const std::exception &error) {
        std::error_code ignored;
        if (!staging.empty()) {
            fs::remove_all(staging, ignored);
        }
        if (!installed.empty()) {
            fs::remove_all(installed, ignored);
        }
        return { false, trim_ascii(error.what()) };
    }
}

PluginActionResult set_plugin_enabled(std::string_view raw_id, bool enabled)
{
    const std::string id = trim_ascii(raw_id);
    if (!plugin_identifier_is_safe(id)) {
        return { false, "插件 ID 无效" };
    }
    try {
        const auto rows = installations();
        const auto it =
            std::find_if(rows.begin(), rows.end(), [&](const PluginInstallation &row) { return row.id == id; });
        if (it == rows.end()) {
            return { false, "插件不存在" };
        }
        if (enabled) {
            if (!it->system_plugin) {
                replace_active_link(id, it->package_path);
            }
            set_disabled_marker(id, false);
        } else {
            remove_active_link(id);
            set_disabled_marker(id, true);
        }
        set_installation_state(id, "pending_restart", "", enabled);
        return { true, enabled ? "已启用；下次重启生效" : "已停用；下次重启生效" };
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
        const auto rows = installations();
        const auto it =
            std::find_if(rows.begin(), rows.end(), [&](const PluginInstallation &row) { return row.id == id; });
        if (it == rows.end()) {
            return { false, "插件不存在" };
        }
        if (it->system_plugin) {
            return { false, "镜像内插件只能停用，不能卸载" };
        }
        for (const PluginInstallation &candidate : rows) {
            if (candidate.id == id || !candidate.enabled || candidate.status == "pending_uninstall") {
                continue;
            }
            try {
                const PluginPackage package = read_plugin_package(candidate.package_path);
                if (std::find(package.dependencies.begin(), package.dependencies.end(), id) !=
                    package.dependencies.end()) {
                    return { false, "仍有已启用插件依赖此插件：" + candidate.id };
                }
            } catch (const std::exception &) {
                return { false, "无法验证依赖插件：" + candidate.id };
            }
        }
        remove_active_link(id);
        set_disabled_marker(id, false);
        set_installation_state(id, "pending_uninstall", "", false);
        return { true, "已安排卸载；下次重启删除包，业务数据保留" };
    } catch (const std::exception &error) {
        return { false, trim_ascii(error.what()) };
    }
}

json plugin_installations_json()
{
    json out = json::array();
    for (const PluginInstallation &installation : installations()) {
        json item;
        item["id"] = installation.id;
        item["name"] = installation.display_name;
        item["version"] = installation.version;
        item["sdk_abi"] = installation.core_abi;
        item["core_abi"] = installation.core_abi;
        item["status"] = installation.status;
        item["path"] = installation.package_path;
        item["target"] = json({ { "os", installation.target_os }, { "arch", installation.target_arch } });
        item["enabled"] = installation.enabled;
        item["system_plugin"] = installation.system_plugin;
        item["error"] = installation.error_message;
        const auto migrations =
            sql_query_rows(database(), "SELECT migration_id,applied_at FROM plugin_migrations WHERE plugin_id=" +
                                           sql_quote(database(), installation.id) + " ORDER BY migration_id");
        json migration_rows = json::array();
        for (const SqlResultRow &row : migrations) {
            migration_rows.push_back(json({ { "id", row[0].value_or("") }, { "applied_at", row[1].value_or("") } }));
        }
        item["migrations"] = std::move(migration_rows);
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<ActivePlugin> prepare_plugins_for_worker()
{
    std::error_code error;
    const fs::path root = user_plugin_root();
    fs::create_directories(root / "packages", error);
    fs::create_directories(root / "active", error);
    fs::create_directories(root / "disabled", error);
    if (error) {
        throw std::runtime_error("unable to create plugin state directory");
    }
    remove_pending_uninstalls();
    discover_system_packages();
    const std::vector<ActivePlugin> selected = active_plugins(root, system_plugin_root());
    for (const PluginInstallation &installation : installations()) {
        if (installation.enabled && installation.status != "pending_uninstall" &&
            !is_selected(selected, installation.id)) {
            set_installation_state(installation.id, "failed", "plugin is not in the v1 load set", true);
        }
    }
    return selected;
}

void apply_plugin_migrations(const ActivePlugin &plugin)
{
    apply_migrations(plugin);
}

void set_plugin_runtime_state(std::string_view plugin_id, std::string_view status, std::string_view message)
{
    set_installation_state(plugin_id, status, message, status == "active" ? std::optional<bool>{ true } : std::nullopt);
}

} // namespace revlm::plugin
