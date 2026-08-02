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

int expect_preload(const char *module, const char *provider)
{
    const std::string command = "LD_PRELOAD=" + shell_quote(module) + " " +
                                shell_quote(REVLM_TEST_PROVIDER_PRELOAD_PROBE_PATH) + " " + shell_quote(provider);
    FILE *pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        std::cerr << "unable to start provider preload probe\n";
        return 1;
    }
    char output[128]{};
    const std::string line = std::fgets(output, sizeof(output), pipe) == nullptr ? "" : output;
    const int status = ::pclose(pipe);
    const std::string expected = std::string{ provider } + "\n";
    if (status != 0 || line != expected) {
        std::cerr << provider << " provider preload failed: " << line;
        return 1;
    }
    return 0;
}

} // namespace
#endif

int main()
{
#ifdef __APPLE__
    // See preload_symbols_test.cpp: the production mechanism is Linux ELF.
    return 0;
#else
    return expect_preload(REVLM_TEST_OPENAI_PLUGIN_PATH, "openai") ||
                   expect_preload(REVLM_TEST_ANTHROPIC_PLUGIN_PATH, "anthropic") ?
               1 :
               0;
#endif
}
