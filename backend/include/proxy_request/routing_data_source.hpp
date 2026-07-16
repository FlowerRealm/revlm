#pragma once

#include <vector>

#include "channels/channels.hpp"
#include "scheduler/scheduler.hpp"

namespace revlm
{

class ProxyRoutingDataSource final : public SchedulerRoutingDataSource {
public:
    ProxyRoutingDataSource() = default;

    std::vector<Channel> list_channels() override
    {
        return ChannelStore::instance().list_channels();
    }
};

} // namespace revlm
