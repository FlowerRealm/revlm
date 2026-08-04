#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

#include "config/config.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"

namespace
{

namespace fs = std::filesystem;

fs::path executable_path(const char *argv0)
{
#ifdef __linux__
    std::vector<char> path(4096, '\0');
    const ssize_t size = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (size > 0) {
        return fs::path{ std::string{ path.data(), static_cast<std::size_t>(size) } };
    }
#endif
    std::error_code error;
    const fs::path absolute = fs::absolute(argv0, error);
    return error ? fs::path{ argv0 } : absolute;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        revlm::init_config(revlm::load_config_from_env());
        revlm::init_database();
        revlm::ensure_schema(revlm::database());

        const fs::path worker = executable_path(argc > 0 ? argv[0] : "revlm").parent_path() / "revlm-worker";
        std::vector<char *> worker_args;
        worker_args.reserve(static_cast<std::size_t>(argc) + 1);
        std::string worker_text = worker.string();
        worker_args.push_back(worker_text.data());
        for (int index = 1; index < argc; ++index) {
            worker_args.push_back(argv[index]);
        }
        worker_args.push_back(nullptr);
        ::execv(worker_text.c_str(), worker_args.data());
        throw std::runtime_error(std::string("unable to start worker: ") + std::strerror(errno));
    } catch (const std::exception &err) {
        std::cerr << "failed to bootstrap revlm: " << err.what() << '\n';
        return 1;
    }
}
