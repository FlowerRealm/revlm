#pragma once

#include <string>
#include <string_view>

namespace revlm
{

std::string trim_ascii(std::string_view value);
std::string lowercase_ascii(std::string_view value);
std::string format_usd_plain_fixed6(std::string_view raw);

} // namespace revlm
