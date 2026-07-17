#include "proxy_request/token_auth.hpp"
#include "users/users.hpp"
#include "users/tokens.hpp"

#include <optional>
#include <string>

#include "util/strings.hpp"

namespace revlm
{
namespace
{

boost::json::object auth_failure()
{
    return boost::json::object{ { "status", false } };
}

std::optional<std::string> extract_token(const ::httplib::Request &req)
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

} // namespace

boost::json::object authenticate_token(const ::httplib::Request &req)
{
    const auto raw_token = extract_token(req);
    if (!raw_token.has_value()) {
        return auth_failure();
    }
    try {
        auto auth = UserStore::instance().tokens().get_token_auth_by_raw_token(*raw_token);
        if (!auth.has_value() || auth->channel_id <= 0) {
            return auth_failure();
        }
        return boost::json::object{
            { "status", true },
            { "auth",
              boost::json::object{
                  { "user_id", auth->user_id },
                  { "token_id", auth->token_id },
                  { "role", auth->role },
                  { "channel_id", auth->channel_id },
              } },
        };
    } catch (const std::exception &) {
        return auth_failure();
    }
}

} // namespace revlm
