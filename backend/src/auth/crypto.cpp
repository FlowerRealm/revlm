#include "auth/crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <stdexcept>

namespace revlm
{

std::string random_bytes(size_t size)
{
    std::string out(size, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), static_cast<int>(out.size())) != 1) {
        throw std::runtime_error("random generation failed");
    }
    return out;
}

std::string base64url_encode(std::string_view input)
{
    if (input.empty()) {
        return {};
    }
    std::string out(4 * ((input.size() + 2) / 3), '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                                  reinterpret_cast<const unsigned char *>(input.data()),
                                  static_cast<int>(input.size()));
    if (n < 0) {
        throw std::runtime_error("base64 encode failed");
    }
    out.resize(static_cast<size_t>(n));
    for (char &ch : out) {
        if (ch == '+') {
            ch = '-';
        } else if (ch == '/') {
            ch = '_';
        }
    }
    while (!out.empty() && out.back() == '=') {
        out.pop_back();
    }
    return out;
}

std::optional<std::string> base64url_decode(std::string_view input)
{
    std::string padded{ input };
    for (char &ch : padded) {
        if (ch == '-') {
            ch = '+';
        } else if (ch == '_') {
            ch = '/';
        }
    }
    switch (padded.size() % 4) {
    case 0:
        break;
    case 2:
        padded.append("==");
        break;
    case 3:
        padded.push_back('=');
        break;
    default:
        return std::nullopt;
    }
    std::string out(3 * (padded.size() / 4), '\0');
    const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                                  reinterpret_cast<const unsigned char *>(padded.data()),
                                  static_cast<int>(padded.size()));
    if (n < 0) {
        return std::nullopt;
    }
    out.resize(static_cast<size_t>(n));
    while (!padded.empty() && padded.back() == '=') {
        padded.pop_back();
        if (!out.empty()) {
            out.pop_back();
        }
    }
    return out;
}

std::string sha256_bytes(std::string_view input)
{
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest.data());
    return std::string{ reinterpret_cast<const char *>(digest.data()), digest.size() };
}

std::string hex_encode(std::string_view bytes)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char ch : bytes) {
        out.push_back(hex[(ch >> 4) & 0xf]);
        out.push_back(hex[ch & 0xf]);
    }
    return out;
}

std::string sha256_hex(std::string_view input)
{
    return hex_encode(sha256_bytes(input));
}

bool constant_time_equal(std::string_view lhs, std::string_view rhs)
{
    return lhs.size() == rhs.size() && CRYPTO_memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

} // namespace revlm
