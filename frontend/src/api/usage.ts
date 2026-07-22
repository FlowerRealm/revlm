import { api } from './client';
import type { APIResponse } from './types';
import { browserTimeZone } from './timezone';

export type UsageWindow = {
  window: string;
  since: string;
  until: string;
  requests: number;
  tokens: number;
  rpm: number;
  tpm: number;
  input_tokens: number;
  output_tokens: number;
  cache_read_tokens: number;
  cache_creation_tokens: number;
  cache_ratio: number;
  first_token_samples: number;
  avg_first_token_latency: number;
  tokens_per_second: number;
  usd: string;
};

type UsageWindowsResponse = {
  time_zone?: string;
  now: string;
  windows: UsageWindow[];
};

export type UsageEvent = {
  id: number;
  time: string;
  request_id: string;
  response_id?: string | null;
  user_id?: number;
  endpoint?: string | null;
  method?: string | null;
  token_id: number;
  channel_id?: number | null;
  model?: string | null;
  model_name?: string | null;
  service_tier?: string | null;
  input_tokens?: number | null;
  cache_read_tokens?: number | null;
  cache_creation_5m_tokens?: number | null;
  cache_creation_1h_tokens?: number | null;
  cache_creation_tokens?: number | null;
  output_tokens?: number | null;
  tier_multiplier?: number;
  channel_multiplier?: number;
  cost_usd: string;
  status_code: number;
  latency_ms: number;
  first_token_latency_ms?: number;
  error_class?: string | null;
  error_message?: string | null;
  is_stream: boolean;
};

type UsageEventsResponse = {
  events: UsageEvent[];
  next_before_id?: number | null;
};

export type UsageTimeSeriesPoint = {
  bucket: string;
  requests: number;
  tokens: number;
  usd: number;
  cache_ratio: number;
  avg_first_token_latency: number;
  tokens_per_second: number;
};

type UsageTimeSeriesResponse = {
  time_zone?: string;
  start: string;
  end: string;
  granularity: 'hour' | 'day';
  points: UsageTimeSeriesPoint[];
};

export async function getUsageWindows(start?: string, end?: string, tokenID?: number, allTime?: boolean) {
  const res = await api.get<APIResponse<UsageWindowsResponse>>('/api/request/windows', {
    params: {
      start: start || undefined,
      end: end || undefined,
      token_id: tokenID || undefined,
      all_time: allTime ? true : undefined,
      tz: browserTimeZone(),
    },
  });
  return res.data;
}

export async function getUsageEvents(params: {
  limit?: number;
  before_id?: number;
  start?: string;
  end?: string;
  token_id?: number;
  index?: string;
  q?: string;
  q_key?: string;
  q_model?: string;
}) {
  const res = await api.get<APIResponse<UsageEventsResponse>>('/api/request/events', {
    params: {
      ...params,
      tz: browserTimeZone(),
    },
  });
  return res.data;
}

export async function getUsageTimeSeries(
  start?: string,
  end?: string,
  granularity?: 'hour' | 'day',
  tokenID?: number,
  allTime?: boolean
) {
  const res = await api.get<APIResponse<UsageTimeSeriesResponse>>('/api/request/timeseries', {
    params: {
      start: start || undefined,
      end: end || undefined,
      granularity: granularity || undefined,
      token_id: tokenID || undefined,
      all_time: allTime ? true : undefined,
      tz: browserTimeZone(),
    },
  });
  return res.data;
}

export type UsageEventDetail = {
  event_id: number;
  pricing_breakdown?: UsageEventPricingBreakdown;
};

export type UsageEventPricingBreakdown = {
  model_public_id?: string | null;
  service_tier?: string | null;

  input_tokens_total: number;
  input_tokens_cache_read: number;
  input_tokens_cache_creation: number;
  input_tokens_cache_creation_5m: number;
  input_tokens_cache_creation_1h: number;
  input_tokens_billable: number;
  output_tokens_total: number;

  tier_multiplier: number;
  channel_multiplier: number;
  final_cost_usd: string;
};

export async function getUsageEventDetail(eventID: number, tokenID?: number) {
  const res = await api.get<APIResponse<UsageEventDetail>>(`/api/request/events/${eventID}/detail`, {
    params: {
      token_id: tokenID || undefined,
    },
  });
  return res.data;
}
