#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <thread>

#include "config/config.hpp"
#include "server/http_server.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"

namespace
{

std::atomic_bool running{ true };
std::atomic_bool shutdown_requested{ false };

void stop_server(int)
{
    shutdown_requested.store(true);
}

} // namespace

int main()
{
    std::signal(SIGINT, stop_server);
    std::signal(SIGTERM, stop_server);

    try {
        revlm::init_config(revlm::load_config_from_env());
        revlm::init_database();
        revlm::ensure_schema(revlm::database());
        std::cerr << "database schema ready\n";
        revlm::HttpServer server;
        int exit_code = 0;
        std::atomic_bool server_done{ false };
        std::thread server_thread([&] {
            exit_code = server.run(running);
            server_done.store(true);
        });

        while (!shutdown_requested.load() && !server_done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (shutdown_requested.load()) {
            server.drain();
            std::cerr << "revlm C++ skeleton draining; readyz returns 503 for "
                      << revlm::config().shutdown_grace_seconds << "s\n";
            std::this_thread::sleep_for(std::chrono::seconds(revlm::config().shutdown_grace_seconds));
            running.store(false);
        }
        if (server_done.load()) {
            running.store(false);
        }
        server_thread.join();
        return exit_code;
    } catch (const std::exception &err) {
        std::cerr << "failed to start revlm C++ skeleton: " << err.what() << '\n';
        return 1;
    }
}
