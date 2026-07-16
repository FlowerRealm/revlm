#pragma once

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "config/config.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"

namespace revlm::test
{
namespace detail
{

inline std::string trim_ascii(std::string value)
{
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\n' || value[begin] == '\r')) {
        ++begin;
    }
    return value.substr(begin);
}

inline std::string sanitize_token(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        const bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        out.push_back(keep ? ch : '-');
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out.empty() ? "mysql-test" : out;
}

inline bool run_command(std::string_view command)
{
    return std::system(std::string{ command }.c_str()) == 0;
}

inline std::string capture_command(std::string_view command)
{
    FILE *pipe = ::popen(std::string{ command }.c_str(), "r");
    if (pipe == nullptr) {
        return {};
    }
    std::string out;
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        out += buffer;
    }
    const int status = ::pclose(pipe);
    if (status != 0) {
        return {};
    }
    return trim_ascii(std::move(out));
}

inline std::string published_mysql_port(std::string_view container_name)
{
    const std::string output =
        capture_command("docker port " + std::string{ container_name } + " 3306/tcp 2>/dev/null");
    if (output.empty()) {
        return {};
    }
    const size_t colon = output.rfind(':');
    if (colon == std::string::npos || colon + 1 >= output.size()) {
        return {};
    }
    return output.substr(colon + 1);
}

} // namespace detail

struct MysqlTestEnv {
    std::string dsn;
    std::string container_name;
    bool owns_container = false;

    MysqlTestEnv() = default;
    MysqlTestEnv(const MysqlTestEnv &) = delete;
    MysqlTestEnv &operator=(const MysqlTestEnv &) = delete;

    MysqlTestEnv(MysqlTestEnv &&other) noexcept
        : dsn(std::move(other.dsn))
        , container_name(std::move(other.container_name))
        , owns_container(other.owns_container)
    {
        other.owns_container = false;
    }

    MysqlTestEnv &operator=(MysqlTestEnv &&other) noexcept
    {
        if (this == &other) {
            return *this;
        }
        cleanup();
        dsn = std::move(other.dsn);
        container_name = std::move(other.container_name);
        owns_container = other.owns_container;
        other.owns_container = false;
        return *this;
    }

    ~MysqlTestEnv()
    {
        cleanup();
    }

    void cleanup()
    {
        if (!owns_container || container_name.empty()) {
            return;
        }
        (void)detail::run_command("docker rm -f " + container_name + " >/dev/null 2>&1");
        owns_container = false;
    }
};

inline std::optional<MysqlTestEnv> prepare_mysql_test_env(std::string_view label)
{
    const char *dsn = std::getenv("REVLM_TEST_MYSQL_DSN");
    if (dsn != nullptr && dsn[0] != '\0') {
        MysqlTestEnv env;
        env.dsn = dsn;
        return env;
    }

    if (!detail::run_command("docker version >/dev/null 2>&1")) {
        std::cout << "REVLM_TEST_MYSQL_DSN not set and docker unavailable; skipping " << label << '\n';
        return std::nullopt;
    }

    const std::string suffix =
        std::to_string(::getpid()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string token = detail::sanitize_token(label);
    const std::string container_name = "tmp-revlm-" + token + "-mysql-" + suffix;
    const std::string database_name = "tmp_" + token + "_" + std::to_string(::getpid());
    const std::string root_password = "tmp-pass-" + std::to_string(::getpid());

    MysqlTestEnv env;
    env.container_name = container_name;
    env.owns_container = true;

    const std::string run_command = "docker run -d --rm --name " + container_name +
                                    " -e MYSQL_ROOT_PASSWORD=" + root_password +
                                    " -e MYSQL_ROOT_HOST=% -e MYSQL_DATABASE=" + database_name +
                                    " -p 127.0.0.1::3306 mysql:8.0 "
                                    "--character-set-server=utf8mb4 --collation-server=utf8mb4_unicode_ci";
    if (!detail::run_command(run_command + " >/dev/null")) {
        env.cleanup();
        throw std::runtime_error("start tmp mysql container failed");
    }

    const std::string port = detail::published_mysql_port(container_name);
    if (port.empty()) {
        env.cleanup();
        throw std::runtime_error("resolve tmp mysql port failed");
    }

    env.dsn = "root:" + root_password + "@tcp(127.0.0.1:" + port + ")/" + database_name;

    for (int attempt = 0; attempt < 90; ++attempt) {
        try {
            auto db = make_database(env.dsn);
            if (sql_query_one(*db, "SELECT 1").value_or("") == "1") {
                return env;
            }
        } catch (const std::exception &) {
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    env.cleanup();
    throw std::runtime_error("tmp mysql container did not become ready");
}

inline void install_test_runtime(Config cfg)
{
    reset_config_for_test(std::move(cfg));
    reset_database_for_test();
    init_database();
}

} // namespace revlm::test
