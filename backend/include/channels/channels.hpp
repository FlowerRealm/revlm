#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "util/json.hpp"

namespace odb
{
class database;
}

namespace revlm
{

#pragma db object table("channels")
class Channel {
public:
    Channel() = default;
    Channel(long long id, std::string name, bool status, int priority, std::string base_url, std::string api_key = {});

#pragma db id auto
    long long id = 0;
    std::string name;
    bool status = true;
    int priority = 0;
    std::string base_url;
    std::string api_key;
};

class ChannelStore {
public:
    static ChannelStore &instance();

    std::vector<Channel> list_channels();
    std::optional<Channel> find_channel(long long id);
    bool create_channel(Channel &channel);
    bool update_channel(Channel &channel);
    bool delete_channel(Channel &channel);

    ChannelStore(const ChannelStore &) = delete;
    ChannelStore &operator=(const ChannelStore &) = delete;

private:
    friend void reset_stores_for_test();
    ChannelStore();
    static void reset_instance();

    odb::database &db_;
};

struct ChannelParsedRequest {
    std::string_view method;
    std::string_view path;
    std::string_view target;
};

json channel_route(std::string_view raw_request, std::string_view body, const ChannelParsedRequest &parsed,
                   std::string *set_cookie = nullptr);

} // namespace revlm
