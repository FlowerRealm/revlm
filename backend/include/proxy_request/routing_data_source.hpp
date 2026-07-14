#pragma once

#include <memory>
#include <vector>

#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "scheduler/scheduler.hpp"
#include "store/database.hpp"

namespace revlm
{

class ProxyRoutingDataSource final : public SchedulerRoutingDataSource {
public:
    explicit ProxyRoutingDataSource(odb::database &db)
        : channel_store_(db)
        , group_store_(db)
    {
    }

    std::vector<Channel> list_channels() override
    {
        return channel_store_.list_channels();
    }

    std::vector<ChannelGroup> list_channel_groups() override
    {
        return group_store_.list_channel_groups();
    }

private:
    ChannelStore channel_store_;
    ChannelGroupStore group_store_;
};

} // namespace revlm
