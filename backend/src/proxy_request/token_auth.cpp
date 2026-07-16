#include "proxy_request/token_auth.hpp"
#include "users/users.hpp"
#include "users/tokens.hpp"

#include <cctype>
#include <string>
#include "util/strings.hpp"

namespace revlm
{
namespace
{

std::optional<std::string> extract_token_from_httplib_request(const ::httplib::Request &req)
{
    std::string authorization = trim_ascii(req.get_header_value("Authorization"));
    if (!authorization.empty()) {
        const size_t sep = authorization.find(' ');
        if (sep != std::string::npos) {
            const std::string scheme = lowercase_ascii(trim_ascii(authorization.substr(0, sep)));
            const std::string token = trim_ascii(authorization.substr(sep + 1));
            if (scheme == "bearer" && !token.empty()) {
                return token;
            }
        }
    }
    const std::string api_key = trim_ascii(req.get_header_value("x-api-key"));
    if (!api_key.empty()) {
        return api_key;
    }
    return std::nullopt;
}

std::string_view header_value(std::string_view request, std::string_view name)
{
    size_t line_start = 0;
    while (line_start < request.size()) {
        const size_t line_end = request.find("\r\n", line_start);
        if (line_end == std::string_view::npos || line_end == line_start) {
            break;
        }
        std::string_view line = request.substr(line_start, line_end - line_start);
        line_start = line_end + 2;
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        std::string_view key = line.substr(0, colon);
        if (key.size() != name.size()) {
            continue;
        }
        bool equal = true;
        for (size_t i = 0; i < key.size(); ++i) {
            char a = key[i];
            char b = name[i];
            if (a >= 'A' && a <= 'Z') {
                a = static_cast<char>(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = static_cast<char>(b - 'A' + 'a');
            }
            if (a != b) {
                equal = false;
                break;
            }
        }
        if (!equal) {
            continue;
        }
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
            value.remove_suffix(1);
        }
        return value;
    }
    return {};
}

std::optional<std::string> extract_token_from_raw_request(std::string_view raw_request)
{
    std::string authorization = trim_ascii(header_value(raw_request, "Authorization"));
    if (!authorization.empty()) {
        const size_t sep = authorization.find(' ');
        if (sep != std::string::npos) {
            const std::string scheme = lowercase_ascii(trim_ascii(authorization.substr(0, sep)));
            const std::string token = trim_ascii(authorization.substr(sep + 1));
            if (scheme == "bearer" && !token.empty()) {
                return token;
            }
        }
    }
    const std::string api_key = trim_ascii(header_value(raw_request, "x-api-key"));
    if (!api_key.empty()) {
        return api_key;
    }
    return std::nullopt;
}

TokenAuthResult auth_failure(int status, std::string message)
{
    TokenAuthResult result;
    result.status = status;
    result.message = std::move(message);
    return result;
}

} // namespace

TokenAuthResult authenticated_token(const ::httplib::Request &req)
{
    const auto raw_token = extract_token_from_httplib_request(req);
    if (!raw_token.has_value()) {
        return auth_failure(401, "未提供 Token");
    }
    try {
        UserStore &users = UserStore::instance();
        TokenStore &store = users.tokens();
        auto auth = store.get_token_auth_by_raw_token(*raw_token);
        if (!auth.has_value()) {
            return auth_failure(401, "Token 无效");
        }
        TokenAuthResult result;
        result.auth = std::move(auth);
        return result;
    } catch (const std::exception &) {
        return auth_failure(502, "鉴权失败");
    }
}

TokenAuthResult authenticated_token(std::string_view raw_request)
{
    const auto raw_token = extract_token_from_raw_request(raw_request);
    if (!raw_token.has_value()) {
        return auth_failure(401, "未提供 Token");
    }
    try {
        UserStore &users = UserStore::instance();
        TokenStore &store = users.tokens();
        auto auth = store.get_token_auth_by_raw_token(*raw_token);
        if (!auth.has_value()) {
            return auth_failure(401, "Token 无效");
        }
        TokenAuthResult result;
        result.auth = std::move(auth);
        return result;
    } catch (const std::exception &) {
        return auth_failure(502, "鉴权失败");
    }
}

} // namespace revlm
