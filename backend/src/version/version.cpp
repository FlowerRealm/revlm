#include "version/version.hpp"

#ifndef REVLM_VERSION
#define REVLM_VERSION "dev"
#endif

#ifndef REVLM_BUILD_DATE
#define REVLM_BUILD_DATE "unknown"
#endif

namespace revlm
{

BuildInfo build_info()
{
    return BuildInfo{ REVLM_VERSION[0] == '\0' ? "dev" : REVLM_VERSION,
                      REVLM_BUILD_DATE[0] == '\0' ? "unknown" : REVLM_BUILD_DATE };
}

} // namespace revlm
