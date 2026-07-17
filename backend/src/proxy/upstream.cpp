#include "proxy/upstream.hpp"

#include "auth/security.hpp"
#include "channels/channels.hpp"
#include "util/json_util.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>
#include "util/strings.hpp"

#include <httplib.h>
#include <netdb.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace revlm
{
namespace
{

constexpr int k_default_upstream_timeout_ms = 30000;
constexpr int k_channel_type_anthropic = 4;

bool channel_type_is_anthropic(int type)
{
    return type == k_channel_type_anthropic;
}

bool iequals(std::string_view left, std::string_view right)
{
    return lowercase_ascii(left) == lowercase_ascii(right);
}

std::string normalize_path(std::string_view raw)
{
    std::string path = trim_ascii(raw);
    if (path.empty()) {
        return "/";
    }
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

std::string join_paths(std::string_view base_path, std::string_view append_path)
{
    const std::string left = normalize_path(base_path);
    const std::string right = normalize_path(append_path);
    if (left == "/") {
        return right;
    }
    if (right == "/") {
        return left;
    }
    return left + right;
}

std::string normalize_upstream_path(const ValidatedBaseUrl &base_url, std::string_view downstream_path)
{
    std::string path = normalize_path(downstream_path);
    if (base_url.base_path == "/v1" && path.rfind("/v1/", 0) == 0) {
        path.erase(0, 3);
        if (path.empty()) {
            path = "/";
        }
    }
    return join_paths(base_url.base_path, path);
}

std::string header_value(const std::vector<UpstreamHeader> &headers, std::string_view name)
{
    for (const UpstreamHeader &header : headers) {
        if (iequals(header.name, name)) {
            return header.value;
        }
    }
    return {};
}

void set_header(std::vector<UpstreamHeader> &headers, std::string_view name, std::string value)
{
    for (UpstreamHeader &header : headers) {
        if (iequals(header.name, name)) {
            header.name = std::string{ name };
            header.value = std::move(value);
            return;
        }
    }
    headers.push_back({ std::string{ name }, std::move(value) });
}

void erase_header(std::vector<UpstreamHeader> &headers, std::string_view name)
{
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [&](const UpstreamHeader &header) { return iequals(header.name, name); }),
                  headers.end());
}

} // namespace

bool is_hop_by_hop_header(std::string_view name)
{
    const std::string lower = lowercase_ascii(name);
    return lower == "host" || lower == "content-length" || lower == "cookie" || lower == "connection" ||
           lower == "proxy-connection" || lower == "keep-alive" || lower == "proxy-authenticate" ||
           lower == "proxy-authorization" || lower == "te" || lower == "trailer" || lower == "transfer-encoding" ||
           lower == "upgrade" || lower == "x-forwarded-for" || lower == "x-forwarded-host" ||
           lower == "x-forwarded-proto" || lower == "x-revlm-remote-ip" || lower == "x-revlm-client-ip";
}

namespace
{

std::vector<UpstreamHeader> copy_headers(const std::vector<UpstreamHeader> &src)
{
    std::vector<UpstreamHeader> out;
    out.reserve(src.size());
    for (const UpstreamHeader &header : src) {
        if (is_hop_by_hop_header(header.name)) {
            continue;
        }
        out.push_back(header);
    }
    return out;
}

std::string unsupported_parameter_name(std::string_view body)
{
    static const std::regex pattern("unsupported parameter[^a-z0-9_]+([a-z0-9_]+)", std::regex_constants::icase);
    std::smatch match;
    const std::string haystack{ body };
    if (std::regex_search(haystack, match, pattern) && match.size() >= 2) {
        return lowercase_ascii(match[1].str());
    }
    return {};
}

bool rewrite_body_field(std::string_view body, std::string_view source_name, std::string_view dest_name,
                        bool keep_destination, std::string &out)
{
    auto doc = json::parse(body);
    if (!doc || !doc->is_object()) {
        return false;
    }
    if (!doc->contains(source_name)) {
        return false;
    }
    json value = static_cast<const json &>(*doc)[source_name];
    doc->erase(source_name);
    if (!keep_destination || !doc->contains(dest_name)) {
        (*doc)[dest_name] = std::move(value);
    }
    out = doc->dump();
    return true;
}

bool remove_body_field(std::string_view body, std::string_view name, std::string &out)
{
    auto doc = json::parse(body);
    if (!doc || !doc->is_object()) {
        return false;
    }
    if (!doc->contains(name)) {
        return false;
    }
    doc->erase(name);
    out = doc->dump();
    return true;
}

} // namespace

std::string build_upstream_url(const ValidatedBaseUrl &base_url, std::string_view downstream_path,
                               std::string_view query)
{
    std::string path = normalize_upstream_path(base_url, downstream_path);
    std::string url = base_url.scheme + "://" + base_url.host;
    if (!base_url.port.empty() && base_url.port != (base_url.scheme == "https" ? "443" : "80")) {
        url += ":" + base_url.port;
    }
    url += path;
    if (!trim_ascii(query).empty()) {
        url += "?";
        url += std::string{ query };
    }
    return url;
}

UpstreamPreparedRequest UpstreamExecutor::prepare(long long channel_id, UpstreamRequest downstream,
                                                  bool retried_unsupported_parameter, bool enforce_ssrf) const
{
    const auto channel = ChannelStore::instance().find_channel(channel_id);
    if (!channel.has_value() || !channel->status) {
        throw std::runtime_error("channel not found");
    }

    UpstreamPreparedRequest prepared;
    prepared.channel_id = channel_id;
    prepared.base_url = validate_upstream_base_url(channel->base_url);
    if (enforce_ssrf) {
        enforce_upstream_ssrf_guard(prepared.base_url);
    }
    prepared.method = downstream.method.empty() ? "POST" : std::move(downstream.method);
    prepared.retried_unsupported_parameter = retried_unsupported_parameter;
    prepared.body = std::move(downstream.body);

    const std::string api_key = trim_ascii(channel->api_key);
    if (api_key.empty()) {
        throw std::runtime_error("channel api key not found");
    }

    if (channel_type_is_anthropic(channel->type)) {
        if (normalize_path(downstream.path) != "/v1/messages") {
            throw std::invalid_argument("anthropic upstream only supports /v1/messages");
        }
        prepared.headers = copy_headers(downstream.headers);
        downstream.headers.clear();
        erase_header(prepared.headers, "Authorization");
        erase_header(prepared.headers, "X-Api-Key");
        erase_header(prepared.headers, "Accept-Encoding");
        set_header(prepared.headers, "Accept-Encoding", "identity");
        if (trim_ascii(header_value(prepared.headers, "anthropic-version")).empty()) {
            set_header(prepared.headers, "anthropic-version", "2023-06-01");
        }
        set_header(prepared.headers, "x-api-key", api_key);
    } else {
        prepared.headers = copy_headers(downstream.headers);
        downstream.headers.clear();
        erase_header(prepared.headers, "Authorization");
        erase_header(prepared.headers, "X-Api-Key");
        erase_header(prepared.headers, "Accept-Encoding");
        set_header(prepared.headers, "Accept-Encoding", "identity");
        set_header(prepared.headers, "Authorization", "Bearer " + api_key);
    }

    prepared.url = build_upstream_url(prepared.base_url, downstream.path, downstream.query);
    return prepared;
}

UpstreamPreparedRequest rewrite_for_unsupported_parameter_retry(const UpstreamPreparedRequest &prepared,
                                                                const UpstreamResponse &response)
{
    if (prepared.retried_unsupported_parameter) {
        throw std::runtime_error("unsupported parameter rewrite already attempted");
    }
    if (response.status_code < 400 || response.status_code >= 500) {
        throw std::runtime_error("unsupported parameter rewrite requires 4xx response");
    }
    const std::string parameter = unsupported_parameter_name(response.body);
    if (parameter.empty()) {
        throw std::runtime_error("unsupported parameter not found");
    }

    std::string body;
    bool ok = false;
    if (parameter == "max_output_tokens") {
        ok = rewrite_body_field(prepared.body, "max_output_tokens", "max_tokens", true, body);
    } else if (parameter == "max_tokens") {
        ok = rewrite_body_field(prepared.body, "max_tokens", "max_output_tokens", false, body);
    } else if (parameter == "max_completion_tokens") {
        ok = rewrite_body_field(prepared.body, "max_completion_tokens", "max_tokens", true, body);
    } else if (parameter == "stream_options") {
        ok = remove_body_field(prepared.body, "stream_options", body);
    }
    if (!ok || body.empty() || body == prepared.body) {
        throw std::runtime_error("unsupported parameter rewrite not applicable");
    }

    UpstreamPreparedRequest retried = prepared;
    retried.body = std::move(body);
    retried.retried_unsupported_parameter = true;
    return retried;
}

UpstreamExecutionResult UpstreamExecutor::execute(long long channel_id, UpstreamRequest downstream,
                                                  const UpstreamTransport &transport, bool enforce_ssrf) const
{
    const auto channel = ChannelStore::instance().find_channel(channel_id);
    if (!channel.has_value() || !channel->status) {
        throw std::runtime_error("channel not found");
    }

    UpstreamExecutionResult result;
    result.request = prepare(channel_id, std::move(downstream), false, enforce_ssrf);
    result.response = transport(result.request);
    if (channel_type_is_anthropic(channel->type)) {
        return result;
    }
    if (result.response.status_code < 400 || result.response.status_code >= 500) {
        return result;
    }
    try {
        UpstreamPreparedRequest retried = rewrite_for_unsupported_parameter_retry(result.request, result.response);
        const UpstreamResponse retry_response = transport(retried);
        if (retry_response.status_code >= 200 && retry_response.status_code < 300) {
            result.request = std::move(retried);
            result.response = retry_response;
            result.rewrote_unsupported_parameter = true;
        }
    } catch (const std::runtime_error &) {
    }
    return result;
}

bool upstream_channel_allows_private_target(std::string_view base_url)
{
    ValidatedBaseUrl parsed;
    try {
        parsed = validate_upstream_base_url(base_url);
    } catch (const std::invalid_argument &) {
        return false;
    }
    std::string lower;
    lower.reserve(parsed.host.size());
    for (char ch : parsed.host) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (lower == "localhost" || lower == "localhost.localdomain") {
        return true;
    }
    in_addr ipv4{};
    if (::inet_pton(AF_INET, parsed.host.c_str(), &ipv4) == 1) {
        return (ntohl(ipv4.s_addr) & 0xFF000000u) == 0x7F000000u;
    }
    in6_addr ipv6{};
    if (::inet_pton(AF_INET6, parsed.host.c_str(), &ipv6) == 1) {
        static const unsigned char k_loopback[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        return std::memcmp(ipv6.s6_addr, k_loopback, 16) == 0;
    }
    return false;
}

namespace
{

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

    ~StreamBridgeState()
    {
        if (!worker.joinable()) {
            return;
        }
        // If the worker itself drops the last shared_ptr while exiting, joining
        // here would deadlock (join self). Detach in that case.
        if (worker.get_id() == std::this_thread::get_id()) {
            worker.detach();
            return;
        }
        worker.join();
    }

    StreamBridgeState() = default;
    StreamBridgeState(const StreamBridgeState &) = delete;
    StreamBridgeState &operator=(const StreamBridgeState &) = delete;
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
        } else {
            if (!state->headers_ready) {
                state->status_code = result->status;
                state->headers = from_httplib_headers(result->headers);
                state->headers_ready = true;
            }
            // Some httplib paths deliver the body via Result instead of content_receiver.
            if (state->chunks.empty() && !result->body.empty()) {
                state->chunks.push_back(result->body);
            }
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

UpstreamTransport make_default_upstream_transport(int timeout_ms, bool allow_private_target)
{
    const int effective_timeout_ms = timeout_ms > 0 ? timeout_ms : k_default_upstream_timeout_ms;
    return [effective_timeout_ms, allow_private_target](const UpstreamPreparedRequest &prepared) {
        return default_upstream_http_transport(prepared, effective_timeout_ms, allow_private_target);
    };
}

UpstreamExecutionResult execute_with_default_transport(const UpstreamExecutor &executor, long long channel_id,
                                                       UpstreamRequest downstream, int timeout_ms,
                                                       bool allow_private_target)
{
    return executor.execute(channel_id, std::move(downstream),
                            make_default_upstream_transport(timeout_ms, allow_private_target), !allow_private_target);
}

UpstreamResponse default_upstream_http_transport(const UpstreamPreparedRequest &prepared, int timeout_ms,
                                                 bool allow_private_target)
{
    return execute_upstream_http_request(prepared, timeout_ms, allow_private_target);
}

UpstreamStreamResponse default_upstream_http_stream_transport(const UpstreamPreparedRequest &prepared, int timeout_ms,
                                                              bool allow_private_target)
{
    return execute_upstream_http_stream_request(prepared, timeout_ms, allow_private_target);
}

} // namespace revlm
