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
                   const revlm::plugin::PluginPlatform &platform)
{
    const fs::path module = root / "backend" / (platform.os + "-" + platform.arch) / ("lib" + id + ".so");
    fs::create_directories(module.parent_path());
    std::ofstream(module) << "test module";
    fs::create_directories(root / "frontend");
    std::ofstream(root / "frontend" / "channel-types.json") << "{\"channel_types\":[]}";

    std::ofstream manifest(root / "plugin.json");
    manifest << "{\n"
             << "  \"format_version\": 1,\n"
             << "  \"id\": \"" << id << "\",\n"
             << "  \"name\": \"" << id << "\",\n"
             << "  \"version\": \"1.0.0\",\n"
             << "  \"sdk_abi\": \"" << revlm::plugin::k_sdk_abi << "\",\n"
             << "  \"requires\": [";
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        if (index != 0) {
            manifest << ", ";
        }
        manifest << "\"" << dependencies[index] << "\"";
    }
    manifest << "],\n"
             << "  \"targets\": [{\"os\": \"" << platform.os << "\", \"arch\": \"" << platform.arch
             << "\", \"module\": \"backend/" << platform.os << "-" << platform.arch << "/lib" << id << ".so\"}],\n"
             << "  \"frontend_schema\": \"frontend/channel-types.json\",\n"
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
        write_package(base, "Base", {}, platform);
        write_package(overlay, "Overlay", { "Base" }, platform);
        write_package(winner, "Winner", {}, platform);
        write_package(cycle_one, "CycleOne", { "CycleTwo" }, platform);
        write_package(cycle_two, "CycleTwo", { "CycleOne" }, platform);
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
            std::cerr << "unexpected active set:";
            for (const auto &plugin : active) {
                std::cerr << ' ' << plugin.package.id;
            }
            std::cerr << '\n';
        }
        if (expect(active.size() == 3, "missing/cyclic packages must stay out of active set") != 0 ||
            expect(active[0].package.id == "Overlay", "active packages must be ordered deterministically") != 0 ||
            expect(active[1].package.id == "Base", "dependency must follow its dependant") != 0 ||
            expect(active[2].package.id == "Winner", "independent packages remain active") != 0 ||
            expect(active[2].module.filename() == "libWinner.so", "selected platform module must be resolved") != 0) {
            return 1;
        }

        if (expect(fs::is_regular_file(overlay / "frontend" / "channel-types.json"),
                   "v1 packages must provide a channel schema") != 0) {
            return 1;
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
