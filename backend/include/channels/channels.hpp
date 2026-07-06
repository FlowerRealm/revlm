#pragma once

#include "config/config.hpp"
#include "server/http_server.hpp"
#include "store/mysql.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace revlm
{

class Channel {
public:
    Channel()
    {
    }
    Channel(long long id, int type, std::string name, bool status, int priority, std::string base_url,
            std::string api_key = {})
        : id(id)
        , type(type)
        , name(name)
        , status(status)
        , priority(priority)
        , base_url(base_url)
        , api_key(std::move(api_key))
    {
    }
    long long id = 0;
    int type; // 1表示openai_responses, 2表示openai_chat, 4表示anthropics
    std::string name;
    bool status; // true 表示开启, false 表示关闭
    int priority = 0;
    std::string base_url;
    std::string api_key;
};

class ChannelStore {
public:
    explicit ChannelStore(MysqlConnection &conn);
    std::vector<Channel> list_channels();
    bool create_channel(Channel &channel);
    bool update_channel(Channel &channel);
    bool delete_channel(Channel &channel);

private:
    MysqlConnection &conn_;
};

struct ChannelAdminParsedRequest {
    std::string_view method;
    std::string_view path;
    std::string_view target;
};

HttpResponse channel_admin_route(std::string_view raw_request, std::string_view body,
                                 const ChannelAdminParsedRequest &parsed, const Config &config,
                                 std::string_view request_id);

} // namespace revlm
