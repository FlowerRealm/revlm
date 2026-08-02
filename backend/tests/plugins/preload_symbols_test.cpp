#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#ifndef __APPLE__
namespace
{
std::string shell_quote(const char *raw)
{
    std::string quoted = "'";
    for (const char ch : std::string{ raw }) {
        if (ch == '\'') {
            quoted += "'\\\"'\\\"'";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted += "'";
    return quoted;
}

} // namespace
#endif

int main()
{
#ifdef __APPLE__
    // The product ABI is Linux ELF only. Mach-O uses two-level binding, so
    // DYLD_INSERT_LIBRARIES cannot replace an already-bound core symbol.
    return 0;
#else
    const char *key = "LD_PRELOAD";
    const std::string command = std::string{ key } + "=" + shell_quote(REVLM_TEST_PRELOAD_OVERRIDE_PATH) + " " +
                                shell_quote(REVLM_TEST_PRELOAD_PROBE_PATH);
    FILE *pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        std::cerr << "unable to start preload probe\n";
        return 1;
    }
    char output[128]{};
    const std::string line = std::fgets(output, sizeof(output), pipe) == nullptr ? "" : output;
    const int status = ::pclose(pipe);
    if (status != 0 || line != "preloaded\n") {
        std::cerr << "symbol preload failed: " << line;
        return 1;
    }
    return 0;
#endif
}
