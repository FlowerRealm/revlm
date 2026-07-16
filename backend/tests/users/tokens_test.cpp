#include "users/tokens.hpp"
#include "util/user_input.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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
    const std::string token = revlm::new_random_token("sk_", 32);
    if (expect(token.rfind("sk_", 0) == 0, "token should use sk_ prefix") != 0 ||
        expect(token.size() > 40, "token should include enough random material") != 0) {
        return 1;
    }

    const std::string hash = revlm::token_hash("sk_test");
    if (expect(hash.size() == 64, "token hash should be hex-encoded SHA-256") != 0 ||
        expect(hash == "12b2820cf1639904311da5771de1e5bb65c77073fdc7c555df395942df42896b",
               "token hash should match SHA-256") != 0 ||
        expect(revlm::token_hash("sk_test") == hash, "token hash should be stable") != 0 ||
        expect(revlm::token_hash("sk_other") != hash, "different token should hash differently") != 0) {
        return 1;
    }

    const std::vector<std::string> groups =
        revlm::normalize_token_channel_groups({ " vip ", "basic", "vip", "", "A-1_b" });
    if (expect(groups.size() == 3, "channel group normalization should trim and dedupe") != 0 ||
        expect(groups[0] == "vip" && groups[1] == "basic" && groups[2] == "A-1_b",
               "channel group normalization should preserve input order") != 0) {
        return 1;
    }

    std::vector<std::string> many;
    for (int i = 0; i < 25; ++i) {
        many.push_back("g" + std::to_string(i));
    }
    if (expect(revlm::normalize_token_channel_groups(many).size() == 20,
               "token channel groups should be capped at 20") != 0) {
        return 1;
    }

    bool invalid_group = false;
    try {
        (void)revlm::normalize_channel_group_name("bad/group");
    } catch (const std::invalid_argument &) {
        invalid_group = true;
    }
    if (expect(invalid_group, "channel group name should reject invalid characters") != 0) {
        return 1;
    }

    return 0;
}
