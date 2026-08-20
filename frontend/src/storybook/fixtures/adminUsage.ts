import type {
  AdminUsageEvent,
  AdminUsageTimeSeriesPoint,
  AdminUsageUser,
  AdminUsageWindow,
  UsageEventDetail,
} from '../../api/admin/usage';

export const adminUsageWindow: AdminUsageWindow = {
  window: '最近 7 天',
  since: '2026-08-12 00:00:00',
  until: '2026-08-19 00:00:00',
  requests: 48210,
  rpm: '4.8',
  avg_first_token_latency: '0.71',
  usd: '2481.30',
};

export const adminTopUsers: AdminUsageUser[] = [
  { user_id: 1, email: 'alice@example.com', role: 'user', status: 1, usd: '640.20' },
  { user_id: 2, email: 'bob@example.com', role: 'user', status: 1, usd: '310.55' },
  { user_id: 3, email: 'carol@example.com', role: 'user', status: 0, usd: '129.90' },
];

export const adminUsageEvents: AdminUsageEvent[] = [
  {
    id: 5001,
    time: '2026-08-19 09:12:03',
    user_id: 1,
    user_email: 'alice@example.com',
    endpoint: '/v1/chat/completions',
    method: 'POST',
    model: 'claude-opus-5',
    model_name: 'Opus 5',
    status_code: 200,
    latency_ms: 2140,
    first_token_latency_ms: 412,
    channel_group_multiplier: 1,
    usage_details: { input_tokens: 812, output_tokens: 236 },
    cost_usd: '0.184',
    channel_id: 3,
    upstream_channel_name: 'anthropic-primary',
    request_id: 'req_8f2a1c',
    response_id: 'resp_8f2a1c',
    error: '',
    error_message: null,
  },
  {
    id: 5002,
    time: '2026-08-19 09:08:41',
    user_id: 4,
    user_email: 'dave@example.com',
    endpoint: '/v1/chat/completions',
    method: 'POST',
    model: 'claude-sonnet-5',
    model_name: 'Sonnet 5',
    status_code: 500,
    latency_ms: 340,
    first_token_latency_ms: 0,
    channel_group_multiplier: 1,
    usage_details: null,
    cost_usd: '0',
    channel_id: 3,
    upstream_channel_name: 'anthropic-primary',
    request_id: 'req_2b91ee',
    response_id: null,
    error: 'upstream_timeout',
    error_message: '上游超时',
  },
];

export const adminUsageEventDetail: UsageEventDetail = {
  event_id: 5001,
  usage_details: { input_tokens: 812, output_tokens: 236, cache_read_tokens: 0 },
};

export const adminUsageTimeSeries: AdminUsageTimeSeriesPoint[] = Array.from({ length: 24 }, (_, hour) => ({
  bucket: `2026-08-19 ${String(hour).padStart(2, '0')}:00`,
  requests: Math.round(800 + 400 * Math.sin(hour / 3)),
  usd: Number((40 + 20 * Math.sin(hour / 3)).toFixed(2)),
  avg_first_token_latency: Number((0.5 + 0.2 * Math.cos(hour / 4)).toFixed(3)),
}));
