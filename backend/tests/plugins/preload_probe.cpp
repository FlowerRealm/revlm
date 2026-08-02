#include "util/strings.hpp"

#include <iostream>

int main()
{
    std::cout << revlm::trim_ascii("  original  ") << '\n';
    return 0;
}
