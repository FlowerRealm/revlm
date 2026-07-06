#pragma once

#include <vector>

#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "scheduler/scheduler.hpp"
#include "store/mysql.hpp"

namespace revlm
{

class ProxyRoutingDataSource final : public SchedulerRoutingDataSource {
public:
    explicit ProxyRoutingDataSource(ChannelStore &channel_store)
        : channel_store_(channel_store)
    {
    }

    explicit ProxyRoutingDataSource(MysqlConnection &conn)
        : channel_store_(conn)
    {
        ChannelGroupStore::instance().reload(conn);
    }

    std::vector<Channel> list_channels() override
    {
        return channel_store_.list_channels();
    }

    std::vector<ChannelGroup> list_channel_groups() override
    {
        return ChannelGroupStore::instance().list_channel_groups();
    }

private:
    ChannelStore channel_store_;
};

} // namespace revlm
