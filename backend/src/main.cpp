#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <thread>

#include "config/config.hpp"
#include "runtime/concurrency.hpp"
#include "runtime/runtime_cache.hpp"
#include "runtime/runtime_workers.hpp"
#include "server/http_server.hpp"
#include "store/migrations.hpp"
#include "version/version.hpp"

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
        auto config = revlm::load_config_from_env();
        revlm::runtime_cache_coordinator().configure(config);
        const revlm::MigrationResult migrations = revlm::apply_migrations(config);
        std::cerr << "database migrations ready applied=" << migrations.applied << " total=" << migrations.total
                  << '\n';
        std::shared_ptr<revlm::CredentialConcurrencyManager> concurrency_manager;
        if (auto created = revlm::make_credential_concurrency_manager(config)) {
            concurrency_manager = std::shared_ptr<revlm::CredentialConcurrencyManager>(std::move(created));
        }
        if (concurrency_manager && concurrency_manager->uses_redis()) {
            concurrency_manager->ping();
        }
        auto shutdown_draining = std::make_shared<std::atomic_bool>(false);
        auto requests_in_flight = std::make_shared<std::atomic_ullong>(0);
        auto auth_resolver = std::make_shared<revlm::AuthResolver>(config);
        auto coordinator = std::make_shared<revlm::RuntimeCoordinator>();
        revlm::install_runtime_worker_registry(revlm::RuntimeWorkerRegistry{
            .auth_resolver = auth_resolver,
            .coordinator = coordinator,
            .concurrency_manager = concurrency_manager,
            .shutdown_draining = shutdown_draining,
            .requests_in_flight = requests_in_flight,
        });
        revlm::HttpServer server(config, revlm::build_info());
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
            shutdown_draining->store(true);
            server.drain();
            std::cerr << "revlm C++ skeleton draining; readyz returns 503 for " << config.shutdown_grace_seconds
                      << "s\n";
            std::this_thread::sleep_for(std::chrono::seconds(config.shutdown_grace_seconds));
            running.store(false);
        }
        if (server_done.load()) {
            running.store(false);
        }
        server_thread.join();
        if (concurrency_manager) {
            concurrency_manager->close();
        }
        revlm::clear_runtime_worker_registry();
        return exit_code;
    } catch (const std::exception &err) {
        revlm::clear_runtime_worker_registry();
        std::cerr << "failed to start revlm C++ skeleton: " << err.what() << '\n';
        return 1;
    }
}
