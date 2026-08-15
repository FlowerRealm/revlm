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

/*
 * One process. There used to be two: a short-lived bootstrap that ran plugin
 * migrations, built an LD_PRELOAD list and then execv'd the real worker. Plugins
 * are now dlopen'd in place (ADR 0007), so there is nothing left to hand to a
 * second process -- and with the fork gone, plugin static initialisers run once
 * instead of twice, which retires the idempotency rule ADR 0006 had to impose on
 * plugin authors.
 */
int main()
{
    std::signal(SIGINT, stop_server);
    std::signal(SIGTERM, stop_server);
    try {
        revlm::init_config(revlm::load_config_from_env());
        revlm::init_database();
        revlm::ensure_schema(revlm::database());

        // Plugins load inside HttpServer::run(), after the core schema is in
        // place and before the listening socket is opened: their migrations must
        // run on top of a current core schema, and their routes must exist before
        // the first request can miss them.
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
            std::cerr << "revlm draining; readyz returns 503 for " << revlm::config().shutdown_grace_seconds << "s\n";
            std::this_thread::sleep_for(std::chrono::seconds(revlm::config().shutdown_grace_seconds));
        }
        running.store(false);
        server_thread.join();
        return exit_code;
    } catch (const std::exception &error) {
        std::cerr << "failed to start revlm: " << error.what() << '\n';
        return 1;
    }
}
