#include "server/http_server.hpp"

#include "config/config.hpp"
#include "server/http_dispatch.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <httplib.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace revlm
{

void write_json(::httplib::Response &res, int status, json body, std::string_view request_id,
                std::string_view set_cookie)
{
    res.status = status;
    res.reason = (status >= 200 && status < 300) ? "OK" : "Error";
    res.set_header("X-Request-Id", std::string{ request_id });
    if (!set_cookie.empty()) {
        res.set_header("Set-Cookie", std::string{ set_cookie });
    }
    res.set_content(serialize(body), "application/json; charset=utf-8");
}

namespace
{

struct ListenAddress {
    std::string host;
    int port = 0;
};

ListenAddress parse_listen_address(const std::string &raw)
{
    if (raw.empty()) {
        throw std::invalid_argument("listen address is empty");
    }

    std::string host = "0.0.0.0";
    std::string port_text = raw;
    const auto colon = raw.rfind(':');
    if (colon != std::string::npos) {
        host = colon == 0 ? "0.0.0.0" : raw.substr(0, colon);
        port_text = raw.substr(colon + 1);
    }
    if (port_text.empty()) {
        throw std::invalid_argument("listen port is empty");
    }

    size_t pos = 0;
    const int port = std::stoi(port_text, &pos, 10);
    if (pos != port_text.size() || port < 0 || port > 65535) {
        throw std::invalid_argument("listen port is invalid");
    }
    return ListenAddress{ host, port };
}

} // namespace

HttpServer::HttpServer()
    : draining_(std::make_shared<std::atomic_bool>(false))
{
}

void HttpServer::drain()
{
    draining_->store(true);
    if (stop_server_) {
        stop_server_();
    }
}

int HttpServer::run(std::atomic_bool &running)
{
    const Config &cfg = config();
    const ListenAddress address = parse_listen_address(cfg.addr);
    auto server = std::make_shared<::httplib::Server>();
    server->set_keep_alive_max_count(1);
    server->set_payload_max_length(static_cast<size_t>(cfg.http_max_body_bytes));
    server->set_pre_routing_handler([this](const ::httplib::Request &req, ::httplib::Response &res) {
        (void)req;
        if (draining_->load()) {
            res.status = 503;
            res.set_content("draining\n", "text/plain; charset=utf-8");
            return ::httplib::Server::HandlerResponse::Handled;
        }
        return ::httplib::Server::HandlerResponse::Unhandled;
    });
    register_http_routes(*server, draining_);

    stop_server_ = [server]() { server->stop(); };

    std::cerr << "revlm listening on " << cfg.addr << '\n';

    std::thread listen_thread([server, address]() {
        if (!server->listen(address.host, address.port)) {
            std::cerr << "httplib listen failed on " << address.host << ':' << address.port << '\n';
        }
    });

    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    server->stop();
    if (listen_thread.joinable()) {
        listen_thread.join();
    }
    stop_server_ = nullptr;
    return 0;
}

} // namespace revlm
