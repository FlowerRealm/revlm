import type { ChannelItem, ChannelTimeSeriesPoint } from '../../api/channels';

export const channelItems: ChannelItem[] = [
  {
    id: 3,
    type: 'channel',
    name: 'anthropic-primary',
    groups: '默认组',
    status: true,
    priority: 0,
    base_url: 'https://api.anthropic.com',
    price_multiplier: 1,
    in_use: true,
    usage: { usd: '842.10', avg_first_token_latency: '0.71' },
    runtime: { available: true, fail_score: 0, banned_active: false, ban_streak: 0 },
  },
  {
    id: 4,
    type: 'channel',
    name: 'anthropic-backup',
    groups: '默认组',
    status: false,
    priority: 1,
    base_url: 'https://api-backup.anthropic.com',
    price_multiplier: 1.1,
    in_use: false,
    usage: { usd: '0', avg_first_token_latency: '0' },
    runtime: {
      available: false,
      fail_score: 5,
      banned_active: true,
      banned_until: '2026-08-19 10:00:00',
      ban_streak: 2,
    },
  },
];

export const channelTimeSeries: ChannelTimeSeriesPoint[] = Array.from({ length: 24 }, (_, hour) => ({
  bucket: `2026-08-19 ${String(hour).padStart(2, '0')}:00`,
  usd: (30 + 10 * Math.sin(hour / 3)).toFixed(2),
  avg_first_token_latency: (0.6 + 0.15 * Math.cos(hour / 4)).toFixed(3),
}));
