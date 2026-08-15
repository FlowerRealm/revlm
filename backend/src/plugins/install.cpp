#include "plugins/host.hpp"

#include "config/config.hpp"
#include "plugins/manifest.hpp"
#include "plugins/scan.hpp"

#include <zip.h>

#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

/*
 * Archive unpacking for `install_plugin_archive`. Uses libzip end to end
 * (zip_source_buffer_create over the in-memory upload, then zip_open_from_source)
 * -- no hand-rolled EOCD/central-directory reader.
 *
 * Order of operations, matching plugin-package-format.md:
 *   1. every entry is checked for a safe relative path, no symlink, no
 *      encryption, and a bounded size, *while* it is being written into a
 *      staging directory -- nothing unsafe is ever written to disk;
 *   2. once staging holds the whole archive, the manifest and the platform
 *      layout are validated against it;
 *   3. only then is staging renamed into packages/<id>, replacing whatever
 *      was there. No backup of the old package, no rollback: a bad new
 *      package is caught before this step, and after it there is nothing
 *      left to roll back to.
 */

namespace revlm::plugin
{
namespace
{

namespace fs = std::filesystem;

// One archive member's path, already checked and normalised.
struct SafeEntryPath {
    fs::path relative;
    bool is_dir = false;
};

// Zip entry names are POSIX paths with '/' separators regardless of the host
// platform; a literal backslash could only be an attempt to smuggle a
// Windows-style separator past the traversal check below, so it is rejected
// outright rather than interpreted.
std::optional<SafeEntryPath> safe_entry_path(std::string_view raw_name)
{
    if (raw_name.empty() || raw_name.find('\0') != std::string_view::npos ||
        raw_name.find('\\') != std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view trimmed = raw_name;
    bool is_dir = false;
    if (trimmed.back() == '/') {
        is_dir = true;
        trimmed.remove_suffix(1);
        if (trimmed.empty()) {
            return std::nullopt;
        }
    }

    const fs::path path{ trimmed };
    if (path.is_absolute()) {
        return std::nullopt;
    }
    for (const fs::path &segment : path) {
        if (segment.empty() || segment == "." || segment == "..") {
            return std::nullopt;
        }
    }
    return SafeEntryPath{ path, is_dir };
}

bool entry_is_symlink(zip_t *archive, zip_uint64_t index)
{
    zip_uint8_t opsys = 0;
    zip_uint32_t attributes = 0;
    if (zip_file_get_external_attributes(archive, index, 0, &opsys, &attributes) != 0) {
        return false;
    }
    if (opsys != ZIP_OPSYS_UNIX) {
        return false;
    }
    const mode_t mode = static_cast<mode_t>(attributes >> 16);
    return S_ISLNK(mode);
}

// Random enough to avoid a collision between two concurrent installs; this
// is not a security boundary, just a scratch directory name.
fs::path make_staging_dir(const fs::path &plugin_dir, std::error_code &error)
{
    std::random_device rd;
    std::mt19937_64 rng(rd());
    char suffix[17];
    std::snprintf(suffix, sizeof(suffix), "%016llx", static_cast<unsigned long long>(rng()));

    const fs::path staging = plugin_dir / ".staging" / (std::string{ "install-" } + suffix);
    fs::create_directories(staging, error);
    return staging;
}

// Cleans up the staging directory on every exit path. After a successful
// publish (fs::rename) the path no longer exists, so this is then a no-op.
struct StagingGuard {
    fs::path path;

    ~StagingGuard()
    {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
};

std::optional<std::string> read_file(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string bytes{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    if (!input.good() && !input.eof()) {
        return std::nullopt;
    }
    return bytes;
}

// Extract every entry of `archive` into `staging`, enforcing path safety, the
// symlink/encryption bans, and the size caps as it goes. Returns empty on
// success, otherwise an error message.
std::string extract_archive(zip_t *archive, const fs::path &staging, std::int64_t per_file_cap, std::int64_t total_cap)
{
    const zip_int64_t num_entries = zip_get_num_entries(archive, 0);
    if (num_entries < 0) {
        return "unable to read archive directory";
    }

    std::int64_t total_uncompressed = 0;

    for (zip_int64_t i = 0; i < num_entries; ++i) {
        zip_stat_t st;
        zip_stat_init(&st);
        if (zip_stat_index(archive, static_cast<zip_uint64_t>(i), 0, &st) != 0) {
            return std::string{ "unable to stat archive entry: " } + zip_strerror(archive);
        }
        if ((st.valid & ZIP_STAT_NAME) == 0 || st.name == nullptr) {
            return "archive entry has no name";
        }
        const std::string entry_name = st.name;

        const auto safe = safe_entry_path(entry_name);
        if (!safe.has_value()) {
            return "archive entry has an unsafe path: " + entry_name;
        }

        if (entry_is_symlink(archive, static_cast<zip_uint64_t>(i))) {
            return "archive entry is a symlink, which is not allowed: " + entry_name;
        }

        if ((st.valid & ZIP_STAT_ENCRYPTION_METHOD) != 0 && st.encryption_method != ZIP_EM_NONE) {
            return "archive entry is encrypted, which is not allowed: " + entry_name;
        }

        const fs::path out_path = staging / safe->relative;

        if (safe->is_dir) {
            std::error_code ec;
            fs::create_directories(out_path, ec);
            if (ec) {
                return "unable to create directory for " + entry_name;
            }
            continue;
        }

        if ((st.valid & ZIP_STAT_SIZE) == 0 ||
            st.size > static_cast<zip_uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return "archive entry has an unknown or unrepresentable size: " + entry_name;
        }
        const auto declared_size = static_cast<std::int64_t>(st.size);
        if (declared_size > per_file_cap) {
            return "archive entry exceeds the per-file size limit: " + entry_name;
        }
        total_uncompressed += declared_size;
        if (total_uncompressed > total_cap) {
            return "archive expands past the configured total size limit";
        }

        std::error_code ec;
        fs::create_directories(out_path.parent_path(), ec);
        if (ec) {
            return "unable to create directory for " + entry_name;
        }

        zip_file_t *member = zip_fopen_index(archive, static_cast<zip_uint64_t>(i), 0);
        if (member == nullptr) {
            return std::string{ "unable to open archive entry: " } + zip_strerror(archive);
        }

        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            zip_fclose(member);
            return "unable to write staged file for " + entry_name;
        }

        char buffer[64 * 1024];
        std::int64_t written = 0;
        for (;;) {
            const zip_int64_t got = zip_fread(member, buffer, sizeof(buffer));
            if (got < 0) {
                zip_fclose(member);
                return "failed reading archive entry: " + entry_name;
            }
            if (got == 0) {
                break;
            }
            written += got;
            // The central directory's declared size is what drove the cap
            // check above; if the actual stream turns out longer, stop
            // trusting the header and enforce the cap on real bytes too.
            if (written > declared_size) {
                zip_fclose(member);
                return "archive entry exceeds its declared size: " + entry_name;
            }
            out.write(buffer, got);
            if (!out) {
                zip_fclose(member);
                return "failed writing staged file for " + entry_name;
            }
        }
        zip_fclose(member);
    }

    return {};
}

} // namespace

bool install_plugin_archive(std::string_view archive_bytes, std::string &error)
{
    const std::int64_t archive_cap = std::max(config().plugin_max_archive_bytes, 0);
    if (archive_bytes.empty()) {
        error = "empty archive";
        return false;
    }
    if (static_cast<std::int64_t>(archive_bytes.size()) > archive_cap) {
        error = "archive exceeds the configured maximum size";
        return false;
    }

    zip_error_t zip_error;
    zip_error_init(&zip_error);

    zip_source_t *source = zip_source_buffer_create(archive_bytes.data(), archive_bytes.size(), 0, &zip_error);
    if (source == nullptr) {
        error = std::string{ "unable to open archive: " } + zip_error_strerror(&zip_error);
        zip_error_fini(&zip_error);
        return false;
    }

    zip_t *archive = zip_open_from_source(source, ZIP_RDONLY, &zip_error);
    if (archive == nullptr) {
        error = std::string{ "unable to open archive: " } + zip_error_strerror(&zip_error);
        zip_error_fini(&zip_error);
        zip_source_free(source);
        return false;
    }
    zip_error_fini(&zip_error);

    // A single file may be as large as the whole archive is allowed to be
    // (the backend .so is typically the largest member). The total budget is
    // capped at a multiple of the archive size as a zip-bomb guard: a
    // legitimate package's uncompressed tree is not many times larger than
    // its compressed upload, since its bulk is a native shared library that
    // barely compresses at all.
    const std::int64_t per_file_cap = archive_cap;
    const std::int64_t total_uncompressed_cap = archive_cap * 4;

    const fs::path plugin_dir = config().plugin_dir;
    std::error_code ec;
    const fs::path staging = make_staging_dir(plugin_dir, ec);
    if (ec) {
        error = "unable to create staging directory: " + ec.message();
        zip_discard(archive);
        return false;
    }
    StagingGuard guard{ staging };

    if (const std::string extract_error = extract_archive(archive, staging, per_file_cap, total_uncompressed_cap);
        !extract_error.empty()) {
        error = extract_error;
        zip_discard(archive);
        return false;
    }
    zip_discard(archive);

    const auto manifest_bytes = read_file(staging / "plugin.json");
    if (!manifest_bytes.has_value()) {
        error = "archive is missing plugin.json";
        return false;
    }
    const auto manifest = parse_manifest(*manifest_bytes, "plugin.json");
    if (!manifest.has_value()) {
        error = manifest.error();
        return false;
    }

    // Layout is validated against the staged tree, before anything is
    // published -- a package missing its platform module or frontend entry
    // never reaches packages/<id>/.
    if (const std::string layout_error = validate_package_layout(staging); !layout_error.empty()) {
        error = manifest->id + ": " + layout_error;
        return false;
    }

    const fs::path packages_dir = plugin_dir / "packages";
    fs::create_directories(packages_dir, ec);
    if (ec) {
        error = "unable to create packages directory: " + ec.message();
        return false;
    }

    const fs::path final_dir = packages_dir / manifest->id;
    // One package per id: replace whatever was there. No backup, no
    // rollback -- the format doc is explicit that a fresh install is the
    // only recovery path if this leaves things in a bad state.
    fs::remove_all(final_dir, ec);
    ec.clear();
    fs::rename(staging, final_dir, ec);
    if (ec) {
        error = "unable to publish package " + manifest->id + ": " + ec.message();
        return false;
    }

    return true;
}

} // namespace revlm::plugin
