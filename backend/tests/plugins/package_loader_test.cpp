#include "plugins/package.hpp"
#include "plugins/packages.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace
{

namespace fs = std::filesystem;

int expect(bool condition, const char *message)
{
    if (condition) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        char pattern[] = "/tmp/revlm-package-loader-XXXXXX";
        char *path = ::mkdtemp(pattern);
        if (path == nullptr) {
            throw std::runtime_error("unable to create package test directory");
        }
        root = path;
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::remove_all(root, error);
    }

    fs::path root;
};

void write_package(const fs::path &root, const std::string &id, const std::vector<std::string> &dependencies,
                   int load_order, const revlm::plugin::PluginPlatform &platform)
{
    const fs::path module = root / "backend" / (platform.os + "-" + platform.arch) / ("lib" + id + ".so");
    fs::create_directories(module.parent_path());
    std::ofstream(module) << "test module";

    std::ofstream manifest(root / "plugin.json");
    manifest << "{\n"
             << "  \"format_version\": 2,\n"
             << "  \"id\": \"" << id << "\",\n"
             << "  \"name\": \"" << id << "\",\n"
             << "  \"version\": \"1.0.0\",\n"
             << "  \"core_abi\": \"" << revlm::plugin::k_core_abi << "\",\n"
             << "  \"requires\": [";
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        if (index != 0) {
            manifest << ", ";
        }
        manifest << "\"" << dependencies[index] << "\"";
    }
    manifest << "],\n"
             << "  \"load_order\": " << load_order << ",\n"
             << "  \"targets\": [{\"os\": \"" << platform.os << "\", \"arch\": \"" << platform.arch
             << "\", \"module\": \"backend/" << platform.os << "-" << platform.arch << "/lib" << id << ".so\"}],\n"
             << "  \"migrations\": []\n"
             << "}\n";
}

} // namespace

int main()
{
    try {
        TemporaryDirectory temporary;
        const revlm::plugin::PluginPlatform platform = revlm::plugin::current_plugin_platform();
        const fs::path system = temporary.root / "system";
        const fs::path user = temporary.root / "user";
        const fs::path base = system / "packages" / "Base" / "1.0.0";
        const fs::path overlay = user / "packages" / "Overlay" / "1.0.0";
        const fs::path winner = user / "packages" / "Winner" / "1.0.0";
        const fs::path cycle_one = system / "packages" / "CycleOne" / "1.0.0";
        const fs::path cycle_two = system / "packages" / "CycleTwo" / "1.0.0";
        write_package(base, "Base", {}, 0, platform);
        write_package(overlay, "Overlay", { "Base" }, 0, platform);
        write_package(winner, "Winner", {}, 5, platform);
        write_package(cycle_one, "CycleOne", { "CycleTwo" }, 0, platform);
        write_package(cycle_two, "CycleTwo", { "CycleOne" }, 0, platform);
        try {
            const auto parsed = revlm::plugin::read_plugin_package(base);
            const auto *module = revlm::plugin::module_for_platform(parsed, platform);
            if (module == nullptr || !fs::is_regular_file(base / module->path)) {
                std::cerr << "test package has no current-platform module: " << platform.os << '/' << platform.arch
                          << '\n';
                return 1;
            }
        } catch (const std::exception &error) {
            std::cerr << "test package did not parse: " << error.what() << '\n';
            return 1;
        }
        fs::create_directories(user / "active");
        fs::create_directory_symlink(overlay, user / "active" / "Overlay");
        fs::create_directory_symlink(winner, user / "active" / "Winner");

        const auto active = revlm::plugin::active_plugins(user, system);
        if (active.size() != 3) {
            std::cerr << "unexpected preload set:";
            for (const auto &plugin : active) {
                std::cerr << ' ' << plugin.package.id;
            }
            std::cerr << '\n';
        }
        if (expect(active.size() == 3, "missing/cyclic packages must stay out of preload") != 0 ||
            expect(active[0].package.id == "Winner", "higher load_order must win between independent modules") != 0 ||
            expect(active[1].package.id == "Overlay", "dependant must be before its dependency") != 0 ||
            expect(active[2].package.id == "Base", "dependency must be after its dependant") != 0 ||
            expect(active[0].module.filename() == "libWinner.so", "selected platform module must be resolved") != 0) {
            return 1;
        }

        fs::create_directories(overlay / "frontend");
        std::ofstream(overlay / "frontend" / "entry.js") << "export {}\n";
        std::ofstream(overlay / "frontend" / "chunk.js") << "export const chunk = true\n";
        const std::string worker_packages = "Overlay\t" + overlay.string() + "\n";
        setenv("REVLM_PRELOADED_PLUGIN_ROOTS", worker_packages.c_str(), 1);
        const auto entry = revlm::plugin::plugin_frontend_file("Overlay", "entry.js");
        const auto chunk = revlm::plugin::plugin_frontend_file("Overlay", "chunk.js");
        if (expect(entry.has_value() && chunk.has_value(), "worker snapshot must serve all frontend assets") != 0 ||
            expect(!revlm::plugin::plugin_frontend_file("Overlay", "../outside.js").has_value(),
                   "frontend asset traversal must be rejected") != 0 ||
            expect(revlm::plugin::plugin_frontend_entries_json().size() == 1,
                   "frontend entries must come from the worker preload snapshot") != 0) {
            return 1;
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
