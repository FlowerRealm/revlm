#pragma once

#include <atomic>
#include <functional>
#include <httplib.h>
#include <memory>
#include <string>
#include <string_view>

#include "util/json.hpp"

namespace revlm
{

// HTTP exit only: serialize json onto the wire response.
void write_json(::httplib::Response &res, int status, json body, std::string_view set_cookie = {});

class HttpServer {
public:
    HttpServer();

    int run(std::atomic_bool &running);
    void drain();

private:
    std::shared_ptr<std::atomic_bool> draining_;
    std::function<void()> stop_server_;
};

std::string handle_http_request(std::string_view request, bool draining);

} // namespace revlm
