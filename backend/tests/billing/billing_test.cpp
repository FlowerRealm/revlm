#include "billing/billing.hpp"

#include <iostream>
#include <stdexcept>

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
    if (expect(revlm::decimal_greater_than_zero("0.000001"),
               "decimal_greater_than_zero should accept positive balances") != 0 ||
        expect(!revlm::decimal_greater_than_zero("0.000000"),
               "decimal_greater_than_zero should reject zero balances") != 0) {
        return 1;
    }

    return 0;
}
