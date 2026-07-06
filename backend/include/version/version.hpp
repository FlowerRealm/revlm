#pragma once

#include <string>

namespace revlm
{

struct BuildInfo {
    std::string version;
    std::string date;
};

BuildInfo build_info();

} // namespace revlm
