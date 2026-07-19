#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace revlm
{

std::string random_bytes(size_t size);
std::string base64url_encode(std::string_view input);
std::optional<std::string> base64url_decode(std::string_view input);
std::string sha256_bytes(std::string_view input);
std::string hex_encode(std::string_view bytes);
std::string sha256_hex(std::string_view input);
bool constant_time_equal(std::string_view lhs, std::string_view rhs);

} // namespace revlm
