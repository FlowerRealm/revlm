#include "server/http_server.hpp"

#include "config/config.hpp"
#include "server/http_dispatch.hpp"

#include <httplib.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace revlm
{
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

HttpResponse http_response(int status, std::string_view status_text, boost::json::value body,
                           std::vector<Header> headers)
{
    HttpResponse out;
    out.status = status;
    out.reason = std::string{ status_text };
    out.body = std::move(body);
    out.content_type = "application/json; charset=utf-8";
    out.headers = std::move(headers);
    return out;
}

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
