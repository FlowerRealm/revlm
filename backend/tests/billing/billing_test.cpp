#include "users/users.hpp"

#include <cmath>
#include <iostream>

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
    if (expect(0.000001 > 0, "positive balances should be greater than zero") != 0 ||
        expect(!(0.0 > 0), "zero balances should not be greater than zero") != 0) {
        return 1;
    }

    return 0;
}
