#include "auth/session.hpp"
#include "auth/users.hpp"
#include "util/user_input.hpp"

#include <iostream>
#include <string>

namespace
{

int expect(bool ok, const char *message)
{
    if (ok) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main()
{
    if (expect(revlm::normalize_username(" Alice09 ") == "Alice09", "username should trim and preserve case") != 0) {
        return 1;
    }

    bool bad_username = false;
    try {
        (void)revlm::normalize_username("bad_name");
    } catch (const std::invalid_argument &) {
        bad_username = true;
    }
    if (expect(bad_username, "username should reject non-alnum characters") != 0) {
        return 1;
    }

    const std::string hash = revlm::hash_password("password123");
    if (expect(hash.rfind("$2b$12$", 0) == 0, "password hash should be bcrypt cost 12") != 0 ||
        expect(revlm::check_password(hash, "password123"), "password should verify") != 0 ||
        expect(!revlm::check_password(hash, "wrong-password"), "wrong password should fail") != 0) {
        return 1;
    }

    const std::string secret = "test-secret";
    const revlm::SessionCookie session = revlm::make_session_cookie(42, secret);
    const std::string cookie = session.value;
    const auto verified = revlm::verify_session_cookie_value(cookie, secret);
    if (expect(verified.has_value() && verified->user_id == 42, "signed session should verify") != 0 ||
        expect(!revlm::verify_session_cookie_value(cookie + "x", secret).has_value(), "tampered session should fail") !=
            0 ||
        expect(!revlm::verify_session_cookie_value(cookie, "other-secret").has_value(), "wrong secret should fail") !=
            0) {
        return 1;
    }
    if (expect(revlm::session_binding_hash(session.key).size() == 64, "session binding hash should be sha256 hex") !=
            0 ||
        expect(revlm::session_binding_hash(session.key) == revlm::session_binding_hash(verified->key),
               "verified session key should address the same server binding") != 0) {
        return 1;
    }

    const std::string request = "GET /api/user/self HTTP/1.1\r\nHost: test\r\n"
                                "Cookie: revlm_session=" +
                                cookie + "\r\n\r\n";
    if (expect(revlm::cookie_value(request, "revlm_session").value_or("") == cookie,
               "cookie parser should extract revlm_session") != 0) {
        return 1;
    }

    const std::string secure_cookie = revlm::set_session_cookie_header(
        cookie, "GET / HTTP/1.1\r\nHost: test\r\nX-Revlm-Remote-Ip: 127.0.0.1\r\nX-Forwarded-Proto: https\r\n\r\n");
    const std::string forged_cookie = revlm::set_session_cookie_header(
        cookie, "GET / HTTP/1.1\r\nHost: test\r\nX-Revlm-Remote-Ip: 203.0.113.10\r\nX-Forwarded-Proto: https\r\n\r\n");
    if (expect(secure_cookie.find("; Secure") != std::string::npos, "trusted proxy https should set secure cookie") !=
            0 ||
        expect(forged_cookie.find("; Secure") == std::string::npos,
               "untrusted forwarded proto should not set secure cookie") != 0) {
        return 1;
    }

    return 0;
}
