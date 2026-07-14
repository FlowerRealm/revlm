#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>
#include <string>
#include <vector>

#include "config/config.hpp"

namespace revlm
{

struct Header {
    std::string name;
    std::string value;
};

struct HttpResponse {
    int status = 200;
    std::string reason;
    std::string body;
    std::string content_type = "text/plain; charset=utf-8";
    std::vector<Header> headers;
};

HttpResponse http_response(int status, std::string_view status_text, std::string_view body,
                           std::string_view content_type, std::string_view request_id,
                           const std::vector<Header> &headers = {}, std::string_view response_id = {});

class HttpServer {
public:
    explicit HttpServer(Config config);

    int run(std::atomic_bool &running);
    void drain();

private:
    Config config_;
    std::shared_ptr<std::atomic_bool> draining_;
    std::function<void()> stop_server_;
};

std::string handle_http_request(std::string_view request, const Config &config, bool draining,
                                std::string_view request_id);

} // namespace revlm
