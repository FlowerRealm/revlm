#include "proxy_response/upstream_http.hpp"

#include <sys/socket.h>

#include <cctype>
#include <sstream>
#include <string>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

bool send_all_fd(int fd, std::string_view data)
{
    while (!data.empty()) {
        const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

} // namespace

ClientWriter client_writer_from_fd(int fd)
{
    return [fd](std::string_view data) { return send_all_fd(fd, data); };
}

bool is_sse_content_type(std::string_view content_type)
{
    const std::string normalized = lowercase_ascii(content_type);
    return normalized.find("text/event-stream") != std::string::npos;
}

HttpResponse make_upstream_http_response(int status, const std::vector<UpstreamHeader> &upstream_headers,
                                         std::string body, std::string_view request_id, std::string_view response_id)
{
    HttpResponse out;
    out.status = status;
    out.reason = (status >= 200 && status < 300) ? "OK" : "Upstream";
    out.content_type = "application/json; charset=utf-8";
    out.body = std::move(body);
    for (const UpstreamHeader &header : upstream_headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "connection" || lower == "transfer-encoding" || lower == "content-length") {
            continue;
        }
        if (lower == "content-type") {
            out.content_type = header.value;
            continue;
        }
        if (lower == "x-request-id" || lower == "x-response-id") {
            continue;
        }
        out.headers.push_back({ header.name, header.value });
    }
    if (!request_id.empty()) {
        out.headers.push_back({ "X-Request-Id", std::string{ request_id } });
    }
    if (!response_id.empty()) {
        out.headers.push_back({ "X-Response-Id", std::string{ response_id } });
    }
    return out;
}

std::string read_remaining_stream(const UpstreamReadHandle &stream)
{
    if (!stream.read) {
        return {};
    }
    std::string out;
    char buffer[8192];
    for (;;) {
        const ssize_t n = stream.read(buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
        out.append(buffer, static_cast<size_t>(n));
    }
    return out;
}

std::string drain_upstream_stream_body(UpstreamStreamResponse &upstream)
{
    std::string out = std::move(upstream.initial_body);
    out += read_remaining_stream(upstream.stream);
    if (upstream.stream.close) {
        upstream.stream.close();
    }
    return out;
}

std::string format_upstream_proxy_response_headers(int status_code, const std::vector<UpstreamHeader> &headers,
                                                   size_t body_size)
{
    std::string text = "HTTP/1.1 " + std::to_string(status_code);
    text += status_code >= 200 && status_code < 300 ? " OK\r\n" : " Upstream\r\n";
    for (const auto &header : headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "connection" || lower == "transfer-encoding" || lower == "content-length") {
            continue;
        }
        text += header.name + ": " + header.value + "\r\n";
    }
    text += "Content-Length: " + std::to_string(body_size) + "\r\n";
    text += "Connection: close\r\n\r\n";
    return text;
}

std::string build_synthetic_stream_response_head(int status, std::string_view content_type, std::string_view request_id,
                                                 std::string_view response_id)
{
    std::ostringstream out;
    out << "HTTP/1.1 " << status << (status >= 200 && status < 300 ? " OK" : " Bad Gateway") << "\r\n"
        << "Content-Type: " << (content_type.empty() ? "text/event-stream; charset=utf-8" : content_type) << "\r\n"
        << "X-Request-Id: " << request_id << "\r\n";
    if (!response_id.empty()) {
        out << "X-Response-Id: " << response_id << "\r\n";
    }
    out << "Connection: close\r\n\r\n";
    return out.str();
}

} // namespace revlm
