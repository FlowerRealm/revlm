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
#include "store/mysql.hpp"
#include "usage/usage_commit_jobs.hpp"
#include "version/version.hpp"

namespace
{

std::atomic_bool running{ true };
std::atomic_bool shutdown_requested{ false };

void stop_server(int)
{
    shutdown_requested.store(true);
}

void run_usage_commit_runtime(const revlm::Config &config, std::atomic_bool &running)
{
    revlm::MysqlConnection conn(config.db_dsn, revlm::mysql_client_multi_statements);
    conn.exec("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
    revlm::UsageCommitJobStore store(conn);
    revlm::UsageFinalizeSink sink(store, config);
    revlm::UsageCommitRuntime runtime(store, sink, config);
    std::cerr << "usage commit runtime started\n";

    const auto drain_once = [&] {
        const auto stale_before =
            std::chrono::system_clock::now() - std::chrono::milliseconds(config.usage_commit_stale_ms);
        runtime.tick(revlm::usage_commit_timestamp_at(stale_before));
    };

    while (running.load()) {
        drain_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(config.usage_commit_poll_ms));
    }

    drain_once();
    std::cerr << "usage commit runtime stopped\n";
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
        auto usage_finalize_queue_depth = std::make_shared<std::atomic_ullong>(0);
        auto usage_finalize_fallback_sync_total = std::make_shared<std::atomic_ullong>(0);
        auto usage_finalize_flush_total = std::make_shared<std::atomic_ullong>(0);
        auto usage_commit_claimed_total = std::make_shared<std::atomic_ullong>(0);
        auto usage_commit_completed_total = std::make_shared<std::atomic_ullong>(0);
        auto usage_commit_dead_letter_total = std::make_shared<std::atomic_ullong>(0);
        auto usage_commit_requeued_total = std::make_shared<std::atomic_ullong>(0);
        auto usage_commit_stale_aborted_total = std::make_shared<std::atomic_ullong>(0);
        auto auth_resolver = std::make_shared<revlm::AuthResolver>(config);
        auto coordinator = std::make_shared<revlm::RuntimeCoordinator>();
        revlm::install_runtime_worker_registry(revlm::RuntimeWorkerRegistry{
            .auth_resolver = auth_resolver,
            .coordinator = coordinator,
            .concurrency_manager = concurrency_manager,
            .shutdown_draining = shutdown_draining,
            .requests_in_flight = requests_in_flight,
            .usage_finalize_queue_depth = usage_finalize_queue_depth,
            .usage_finalize_fallback_sync_total = usage_finalize_fallback_sync_total,
            .usage_finalize_flush_total = usage_finalize_flush_total,
            .usage_commit_claimed_total = usage_commit_claimed_total,
            .usage_commit_completed_total = usage_commit_completed_total,
            .usage_commit_dead_letter_total = usage_commit_dead_letter_total,
            .usage_commit_requeued_total = usage_commit_requeued_total,
            .usage_commit_stale_aborted_total = usage_commit_stale_aborted_total,
        });
        revlm::HttpServer server(config, revlm::build_info());
        int exit_code = 0;
        std::atomic_bool server_done{ false };
        std::thread usage_commit_thread([&] {
            try {
                run_usage_commit_runtime(config, running);
            } catch (const std::exception &err) {
                std::cerr << "usage commit runtime failed: " << err.what() << '\n';
                running.store(false);
                shutdown_requested.store(true);
            }
        });
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
        if (usage_commit_thread.joinable()) {
            usage_commit_thread.join();
        }
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
