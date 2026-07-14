#include <iostream>
#include <string>

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
    const std::string token_payload =
        "{\"access_token\":\"acc-token\",\"refresh_token\":\"ref-token\",\"expires_in\":3600,"
        "\"id_token\":\"eyJhbGciOiJub25lIn0."
        "eyJlbWFpbCI6InVzZXJAZXhhbXBsZS5jb20iLCJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdF8xMjMifX0."
        "\"}";

    if (expect(token_payload.find("\"expires_in\":3600") != std::string::npos,
               "token payload fixture should include numeric expires_in") != 0) {
        return 1;
    }

    return 0;
}
