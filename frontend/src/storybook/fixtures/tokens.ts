import type { TokenChannelGroupOption, UserToken, UserTokenChannelGroup } from '../../api/tokens';

export const userTokens: UserToken[] = [
  { id: 1, name: '默认 Token', status: 1, channel_group_id: 1 },
  { id: 2, name: 'CI 集成', status: 1, channel_group_id: 2 },
  { id: 3, name: '已停用', status: 0, channel_group_id: 1 },
];

export const channelGroupOptions: TokenChannelGroupOption[] = [
  { id: 1, name: '默认组', description: '默认渠道组', status: true, price_multiplier: 1 },
  { id: 2, name: '高优先级', description: '优先路由', status: true, price_multiplier: 1.2 },
];

export const userTokenChannel: UserTokenChannelGroup = {
  token_id: 1,
  channel_group_id: 1,
  allowed_channel_groups: channelGroupOptions,
};
