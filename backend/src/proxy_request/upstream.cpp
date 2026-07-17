#include "proxy_request/upstream.hpp"

#include "proxy_request/http_client.hpp"
#include "util/json_util.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <optional>
#include <regex>
#include <stdexcept>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

constexpr int k_default_upstream_timeout_ms = 30000;

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
    auto doc = parse_json(body);
    if (!doc || !doc->is_object()) {
        return false;
    }
    boost::json::object &root = doc->as_object();
    const auto source_it = root.find(source_name);
    if (source_it == root.end()) {
        return false;
    }
    boost::json::value value = source_it->value();
    root.erase(source_it);
    if (!keep_destination || !root.contains(dest_name)) {
        root[dest_name] = std::move(value);
    }
    out = boost::json::serialize(*doc);
    return true;
}

bool remove_body_field(std::string_view body, std::string_view name, std::string &out)
{
    auto doc = parse_json(body);
    if (!doc || !doc->is_object()) {
        return false;
    }
    boost::json::object &root = doc->as_object();
    if (root.erase(name) == 0) {
        return false;
    }
    out = boost::json::serialize(*doc);
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

UpstreamPreparedRequest UpstreamExecutor::prepare(const SchedulerSelection &selection, UpstreamRequest downstream,
                                                  bool retried_unsupported_parameter, bool enforce_ssrf) const
{
    UpstreamPreparedRequest prepared;
    prepared.selection = selection;
    prepared.base_url = validate_upstream_base_url(selection.base_url);
    if (enforce_ssrf) {
        enforce_upstream_ssrf_guard(prepared.base_url);
    }
    prepared.method = downstream.method.empty() ? "POST" : std::move(downstream.method);
    prepared.retried_unsupported_parameter = retried_unsupported_parameter;
    prepared.body = std::move(downstream.body);

    const std::string api_key = trim_ascii(selection.api_key);
    if (api_key.empty()) {
        throw std::runtime_error("channel api key not found");
    }

    if (selection.channel_type == "anthropic") {
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
        if (selection.openai_organization.has_value() && !trim_ascii(*selection.openai_organization).empty()) {
            set_header(prepared.headers, "OpenAI-Organization", trim_ascii(*selection.openai_organization));
        }
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

UpstreamExecutionResult UpstreamExecutor::execute(const SchedulerSelection &selection, UpstreamRequest downstream,
                                                  const UpstreamTransport &transport, bool enforce_ssrf) const
{
    UpstreamExecutionResult result;
    result.request = prepare(selection, std::move(downstream), false, enforce_ssrf);
    result.response = transport(result.request);
    if (selection.channel_type == "anthropic") {
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

UpstreamTransport make_default_upstream_transport(int timeout_ms, bool allow_private_target)
{
    const int effective_timeout_ms = timeout_ms > 0 ? timeout_ms : k_default_upstream_timeout_ms;
    return [effective_timeout_ms, allow_private_target](const UpstreamPreparedRequest &prepared) {
        return default_upstream_http_transport(prepared, effective_timeout_ms, allow_private_target);
    };
}

UpstreamExecutionResult execute_with_default_transport(const UpstreamExecutor &executor,
                                                       const SchedulerSelection &selection, UpstreamRequest downstream,
                                                       int timeout_ms, bool allow_private_target)
{
    return executor.execute(selection, std::move(downstream),
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
