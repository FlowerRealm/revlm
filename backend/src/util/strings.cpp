#include "util/strings.hpp"

#include <cctype>

namespace revlm
{

std::string trim_ascii(std::string_view value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string{ value.substr(begin, end - begin) };
}

std::string lowercase_ascii(std::string_view value)
{
    std::string out{ value };
    for (char &ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

std::string format_usd_plain_fixed6(std::string_view raw)
{
    std::string text = trim_ascii(raw);
    if (text.empty()) {
        return "0";
    }
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.erase(text.begin());
    }
    const size_t dot = text.find('.');
    std::string integer = dot == std::string::npos ? text : text.substr(0, dot);
    std::string fraction = dot == std::string::npos ? "" : text.substr(dot + 1);
    if (integer.empty()) {
        integer = "0";
    }
    size_t non_zero = 0;
    while (non_zero + 1 < integer.size() && integer[non_zero] == '0') {
        ++non_zero;
    }
    integer.erase(0, non_zero);
    while (!fraction.empty() && fraction.back() == '0') {
        fraction.pop_back();
    }
    std::string out = negative ? "-" : "";
    out += integer;
    if (!fraction.empty()) {
        out += ".";
        out += fraction;
    }
    return out;
}

} // namespace revlm
