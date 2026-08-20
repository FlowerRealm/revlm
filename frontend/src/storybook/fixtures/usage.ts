import type { UsageEvent, UsageEventDetail, UsageTimeSeriesPoint, UsageWindow } from '../../api/usage';
import type { UserToken } from '../../api/tokens';
import type { TopUserView } from '../../pages/usage/usageUtils';

export const usageWindow: UsageWindow = {
  window: '最近 7 天',
  since: '2026-08-12 00:00:00',
  until: '2026-08-19 00:00:00',
  requests: 12480,
  rpm: 1.2,
  first_token_samples: 11960,
  avg_first_token_latency: 0.842,
  usd: '128.44',
};

export const emptyUsageWindow: UsageWindow = {
  ...usageWindow,
  requests: 0,
  rpm: 0,
  first_token_samples: 0,
  avg_first_token_latency: 0,
  usd: '0',
};

export const tokenByID: Record<number, UserToken> = {
  1: { id: 1, name: '默认 Token', status: 1, channel_group_id: 1 },
  2: { id: 2, name: 'CI 集成', status: 1, channel_group_id: 1 },
};

export const usageEvents: UsageEvent[] = [
  {
    id: 1001,
    time: '2026-08-19 09:12:03',
    request_id: 'req_8f2a1c',
    response_id: 'resp_8f2a1c',
    user_id: 1,
    endpoint: '/v1/chat/completions',
    method: 'POST',
    token_id: 1,
    channel_id: 3,
    model: 'claude-opus-5',
    model_name: 'Opus 5',
    channel_group_multiplier: 1,
    usage_details: { input_tokens: 812, output_tokens: 236 },
    cost_usd: '0.184',
    status_code: 200,
    latency_ms: 2140,
    first_token_latency_ms: 412,
    error_message: null,
  },
  {
    id: 1002,
    time: '2026-08-19 09:08:41',
    request_id: 'req_2b91ee',
    response_id: null,
    user_id: 1,
    endpoint: '/v1/chat/completions',
    method: 'POST',
    token_id: 2,
    channel_id: 3,
    model: 'claude-sonnet-5',
    model_name: 'Sonnet 5',
    channel_group_multiplier: 1,
    usage_details: null,
    cost_usd: '0',
    status_code: 500,
    latency_ms: 340,
    first_token_latency_ms: undefined,
    error_message: '上游超时',
  },
];

export const usageEventDetail: UsageEventDetail = {
  event_id: 1001,
  usage_details: { input_tokens: 812, output_tokens: 236, cache_read_tokens: 0 },
};

export const usageTimeSeries: UsageTimeSeriesPoint[] = Array.from({ length: 24 }, (_, hour) => ({
  bucket: `2026-08-19 ${String(hour).padStart(2, '0')}:00`,
  requests: Math.round(200 + 150 * Math.sin(hour / 3)),
  usd: Number((2 + 1.5 * Math.sin(hour / 3)).toFixed(2)),
  avg_first_token_latency: Number((0.5 + 0.2 * Math.cos(hour / 4)).toFixed(3)),
}));

export const topUsers: TopUserView[] = [
  { user_id: 1, email: 'alice@example.com', role: 'user', status: 1, usd: '64.20' },
  { user_id: 2, email: 'bob@example.com', role: 'user', status: 1, usd: '31.05' },
  { user_id: 3, email: 'carol@example.com', role: 'user', status: 0, usd: '12.90' },
];
