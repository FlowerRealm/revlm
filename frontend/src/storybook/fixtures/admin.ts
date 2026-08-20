import type { AdminDashboard } from '../../api/admin/dashboard';
import type { AdminUser } from '../../api/admin/users';
import type { AdminChannelGroup, AdminChannelGroupDetail } from '../../api/admin/channelGroups';

export const adminDashboard: AdminDashboard = {
  admin_time_zone: 'Asia/Shanghai',
  stats: {
    users_count: 128,
    channels_count: 6,
    endpoints_count: 14,
    requests_today: 4820,
    tokens_today: 210,
    input_tokens_today: 1_840_000,
    output_tokens_today: 512_000,
    cost_today: '248.13',
  },
};

export const adminUsers: AdminUser[] = [
  { id: 1, email: 'alice@example.com', username: 'alice', role: 'user', status: 1, balance_usd: 12.4 },
  { id: 2, email: 'root@example.com', username: 'root', role: 'root', status: 1, balance_usd: 0 },
  { id: 3, email: 'dave@example.com', username: 'dave', role: 'user', status: 0, balance_usd: 0 },
];

export const adminChannelGroups: AdminChannelGroup[] = [
  {
    id: 1,
    name: '默认组',
    description: '默认渠道组',
    price_multiplier: 1,
    status: true,
    type: 'channel',
    is_default: true,
    pointer_channel_id: 3,
    pointer_channel_name: 'anthropic-primary',
    pointer_pinned: false,
  },
  {
    id: 2,
    name: '高优先级',
    description: null,
    price_multiplier: 1.2,
    status: true,
    type: 'channel',
    is_default: false,
  },
];

export const adminChannelGroupDetail: AdminChannelGroupDetail = {
  group: adminChannelGroups[0],
  members: [
    {
      member_id: 10,
      parent_group_id: 1,
      member_channel_id: 3,
      member_channel_name: 'anthropic-primary',
      member_channel_type: 'channel',
      member_channel_status: 1,
      priority: 0,
      promotion: false,
    },
  ],
  channels: [{ id: 3, name: 'anthropic-primary', type: 'channel' }],
};
