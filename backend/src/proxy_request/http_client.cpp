#include "proxy_request/http_client.hpp"

#include "auth/security.hpp"

#include <httplib.h>

#include <arpa/inet.h>
#include <netdb.h>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace revlm
{
namespace
{

constexpr int k_default_upstream_timeout_ms = 30000;

bool iequals(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

int resolve_port(const ValidatedBaseUrl &base_url)
{
    if (!base_url.port.empty()) {
        return std::stoi(base_url.port);
    }
    return base_url.scheme == "https" ? 443 : 80;
}

std::string request_target_path(const UpstreamPreparedRequest &prepared)
{
    const size_t authority_pos = prepared.url.find("://");
    const size_t path_pos = authority_pos == std::string::npos ? prepared.url.find('/') :
                                                                 prepared.url.find('/', authority_pos + 3);
    return path_pos == std::string::npos ? std::string{ "/" } : prepared.url.substr(path_pos);
}

void assert_resolved_addresses_allowed(const ValidatedBaseUrl &base_url, bool allow_private_target)
{
    if (allow_private_target) {
        return;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *result = nullptr;
    const std::string port = base_url.port.empty() ? (base_url.scheme == "https" ? "443" : "80") : base_url.port;
    if (::getaddrinfo(base_url.host.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr) {
        throw std::runtime_error("upstream DNS resolution failed");
    }
    bool allowed = false;
    for (addrinfo *item = result; item != nullptr; item = item->ai_next) {
        if (is_safe_upstream_sockaddr(item->ai_addr, item->ai_addrlen)) {
            allowed = true;
            break;
        }
    }
    ::freeaddrinfo(result);
    if (!allowed) {
        throw std::runtime_error("upstream resolved to blocked address");
    }
}

std::unique_ptr<httplib::Client> make_client(const ValidatedBaseUrl &base_url, int timeout_ms)
{
    const int port = resolve_port(base_url);
    std::string endpoint = base_url.scheme + "://" + base_url.host;
    const bool default_port = (base_url.scheme == "https" && port == 443) || (base_url.scheme == "http" && port == 80);
    if (!default_port) {
        endpoint += ":" + std::to_string(port);
    }
    auto client = std::make_unique<httplib::Client>(endpoint);
    client->set_connection_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
    client->set_read_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
    client->set_write_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
    client->set_follow_location(false);
    return client;
}

httplib::Headers to_httplib_headers(const std::vector<UpstreamHeader> &headers)
{
    httplib::Headers out;
    for (const UpstreamHeader &header : headers) {
        if (iequals(header.name, "Host") || iequals(header.name, "Content-Length") ||
            iequals(header.name, "Connection")) {
            continue;
        }
        out.emplace(header.name, header.value);
    }
    return out;
}

std::string content_type_from_headers(const std::vector<UpstreamHeader> &headers)
{
    for (const UpstreamHeader &header : headers) {
        if (iequals(header.name, "Content-Type")) {
            return header.value;
        }
    }
    return "application/json";
}

std::vector<UpstreamHeader> from_httplib_headers(const httplib::Headers &headers)
{
    std::vector<UpstreamHeader> out;
    out.reserve(headers.size());
    for (const auto &header : headers) {
        out.push_back({ header.first, header.second });
    }
    return out;
}

httplib::Request build_request(const UpstreamPreparedRequest &prepared)
{
    httplib::Request req;
    req.method = prepared.method;
    req.path = request_target_path(prepared);
    req.headers = to_httplib_headers(prepared.headers);
    req.body = prepared.body;
    if (!prepared.body.empty()) {
        req.set_header("Content-Type", content_type_from_headers(prepared.headers));
    }
    return req;
}

struct StreamBridgeState {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> chunks;
    size_t chunk_offset = 0;
    bool headers_ready = false;
    bool done = false;
    bool worker_error = false;
    int status_code = 0;
    std::vector<UpstreamHeader> headers;
    std::thread worker;
};

ssize_t stream_bridge_read(const std::shared_ptr<StreamBridgeState> &state, char *out, size_t size)
{
    std::unique_lock<std::mutex> lock(state->mu);
    for (;;) {
        if (!state->chunks.empty()) {
            while (!state->chunks.empty() && state->chunk_offset >= state->chunks.front().size()) {
                state->chunks.pop_front();
                state->chunk_offset = 0;
            }
            if (!state->chunks.empty()) {
                const std::string &front = state->chunks.front();
                const size_t available = front.size() - state->chunk_offset;
                const size_t n = std::min(size, available);
                std::memcpy(out, front.data() + state->chunk_offset, n);
                state->chunk_offset += n;
                return static_cast<ssize_t>(n);
            }
        }
        if (state->done) {
            return 0;
        }
        state->cv.wait_for(lock, std::chrono::milliseconds(100));
        if (state->worker_error && state->chunks.empty()) {
            return -1;
        }
    }
}

} // namespace

UpstreamResponse execute_upstream_http_request(const UpstreamPreparedRequest &prepared, int timeout_ms,
                                               bool allow_private_target)
{
    const int effective_timeout_ms = timeout_ms > 0 ? timeout_ms : k_default_upstream_timeout_ms;
    if (!allow_private_target) {
        enforce_upstream_ssrf_guard(prepared.base_url);
    }
    assert_resolved_addresses_allowed(prepared.base_url, allow_private_target);

    auto client = make_client(prepared.base_url, effective_timeout_ms);
    httplib::Request req = build_request(prepared);
    httplib::Response res;
    httplib::Error err = httplib::Error::Success;
    if (!client->send(req, res, err) || err != httplib::Error::Success) {
        throw std::runtime_error("upstream request failed");
    }

    UpstreamResponse response;
    response.status_code = res.status;
    response.headers = from_httplib_headers(res.headers);
    response.body = std::move(res.body);
    return response;
}

UpstreamStreamResponse execute_upstream_http_stream_request(const UpstreamPreparedRequest &prepared, int timeout_ms,
                                                            bool allow_private_target)
{
    const int effective_timeout_ms = timeout_ms > 0 ? timeout_ms : k_default_upstream_timeout_ms;
    if (!allow_private_target) {
        enforce_upstream_ssrf_guard(prepared.base_url);
    }
    assert_resolved_addresses_allowed(prepared.base_url, allow_private_target);

    auto state = std::make_shared<StreamBridgeState>();
    auto client = make_client(prepared.base_url, effective_timeout_ms);
    httplib::Request req = build_request(prepared);
    req.response_handler = [state](const httplib::Response &response) {
        std::lock_guard<std::mutex> lock(state->mu);
        state->status_code = response.status;
        state->headers = from_httplib_headers(response.headers);
        state->headers_ready = true;
        state->cv.notify_all();
        return true;
    };
    req.content_receiver = [state](const char *data, size_t data_length, size_t, size_t) {
        if (data_length == 0) {
            return true;
        }
        std::lock_guard<std::mutex> lock(state->mu);
        state->chunks.emplace_back(data, data_length);
        state->cv.notify_all();
        return true;
    };

    state->worker = std::thread([client = std::move(client), req = std::move(req), state]() mutable {
        const httplib::Result result = client->send(req);
        std::lock_guard<std::mutex> lock(state->mu);
        if (!result) {
            state->worker_error = true;
        } else if (!state->headers_ready) {
            state->status_code = result->status;
            state->headers = from_httplib_headers(result->headers);
            state->headers_ready = true;
        }
        state->done = true;
        state->cv.notify_all();
    });

    {
        std::unique_lock<std::mutex> lock(state->mu);
        if (!state->cv.wait_for(lock, std::chrono::milliseconds(effective_timeout_ms),
                                [&] { return state->headers_ready || state->done; })) {
            if (state->worker.joinable()) {
                state->worker.join();
            }
            throw std::runtime_error("upstream stream headers timeout");
        }
        if (state->worker_error) {
            state->worker.join();
            throw std::runtime_error("upstream stream request failed");
        }
    }

    UpstreamStreamResponse response;
    response.request = prepared;
    response.status_code = state->status_code;
    response.headers = state->headers;
    response.initial_body.clear();

    response.stream.poll_fd = -1;
    response.stream.read = [state](char *out, size_t size) -> ssize_t { return stream_bridge_read(state, out, size); };
    response.stream.close = [state]() {
        if (state->worker.joinable()) {
            state->worker.join();
        }
    };
    return response;
}

} // namespace revlm
