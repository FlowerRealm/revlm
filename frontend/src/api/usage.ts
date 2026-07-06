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
  cache_read_input_tokens: number;
  cache_creation_input_tokens: number;
  cache_ratio: number;
  first_token_samples: number;
  avg_first_token_latency: number;
  tokens_per_second: number;
  used_usd: string;
  committed_usd: string;
  limit_usd: string;
  remaining_usd: string;
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
  endpoint?: string | null;
  method?: string | null;
  token_id: number;
  upstream_endpoint_id?: number | null;
  state: string;
  model?: string | null;
  requested_service_tier?: string | null;
  service_tier?: string | null;
  service_tier_downgraded: boolean;
  service_tier_downgrade_reason?: string | null;
  input_tokens?: number | null;
  cache_read_input_tokens?: number | null;
  output_tokens?: number | null;
  cache_creation_input_tokens?: number | null;
  committed_usd: string;
  status_code: number;
  latency_ms: number;
  error_class?: string | null;
  error_message?: string | null;
  is_stream: boolean;
  request_bytes: number;
  response_bytes: number;
  model_mismatch: boolean;
  created_at: string;
  updated_at: string;
};

type UsageEventsResponse = {
  events: UsageEvent[];
  next_before_id?: number | null;
};

export type UsageTimeSeriesPoint = {
  bucket: string;
  requests: number;
  tokens: number;
  committed_usd: number;
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
  const res = await api.get<APIResponse<UsageWindowsResponse>>('/api/usage/windows', {
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
  const res = await api.get<APIResponse<UsageEventsResponse>>('/api/usage/events', {
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
  const res = await api.get<APIResponse<UsageTimeSeriesResponse>>('/api/usage/timeseries', {
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
  model_check?: UsageEventModelCheck;
};

export type UsageEventModelCheck = {
  forwarded_model?: string | null;
  upstream_response_model?: string | null;
  mismatch: boolean;
};

export type UsageEventPricingBreakdown = {
  model_public_id?: string;
  model_found: boolean;
  owned_by?: string;
  requested_service_tier?: string;
  service_tier?: string;
  service_tier_downgraded: boolean;
  service_tier_downgrade_reason?: string;
  pricing_kind?: string;
  high_context_applied: boolean;
  high_context_threshold_tokens: number;
  high_context_trigger_input_tokens: number;
  effective_service_tier?: string;

  input_tokens_total: number;
  input_tokens_cache_read: number;
  input_tokens_cache_creation: number;
  input_tokens_cache_creation_5m: number;
  input_tokens_cache_creation_1h: number;
  input_tokens_billable: number;
  output_tokens_total: number;

  input_usd_per_1m: string;
  output_usd_per_1m: string;
  cache_read_input_usd_per_1m: string;
  cache_creation_input_usd_per_1m: string;
  cache_creation_1h_input_usd_per_1m: string;

  input_cost_usd: string;
  output_cost_usd: string;
  cache_read_input_cost_usd: string;
  cache_creation_input_cost_usd: string;
  cache_creation_5m_input_cost_usd: string;
  cache_creation_1h_input_cost_usd: string;
  base_cost_usd: string;

  payment_multiplier: string;
  group_name: string;
  group_multiplier: string;
  effective_multiplier: string;

  final_cost_usd: string;
};

export async function getUsageEventDetail(eventID: number, tokenID?: number) {
  const res = await api.get<APIResponse<UsageEventDetail>>(`/api/usage/events/${eventID}/detail`, {
    params: {
      token_id: tokenID || undefined,
    },
  });
  return res.data;
}
