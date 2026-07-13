import { getData } from '../request';
import type { APIResponse } from '../types';

export type AdminUsageWindow = {
  window: string;
  since: string;
  until: string;
  requests: number;
  tokens: number;
  input_tokens: number;
  output_tokens: number;
  cached_tokens: number;
  cache_ratio: string;
  rpm: string;
  tpm: string;
  avg_first_token_latency: string;
  tokens_per_second: string;
  committed_usd: string;
};

export type AdminUsageUser = {
  user_id: number;
  email: string;
  role: string;
  status: number;
  committed_usd: string;
};

export type AdminUsageEvent = {
  id: number;
  time: string;
  user_id: number;
  user_email: string;
  endpoint: string;
  method: string;
  model: string;
  status_code: string;
  latency_ms: string;
  first_token_latency_ms: string;
  tokens_per_second: string;
  input_tokens: string;
  output_tokens: string;
  cached_tokens: string;
  cost_usd: string;
  committed_usd?: string;
  status?: string;
  state_label: string;
  state_badge_class: string;
  service_tier?: string | null;
  is_stream: boolean;
  channel_id: string;
  upstream_channel_name: string;
  request_id: string;
  error: string;
  error_class: string;
  error_message: string;
};

export type AdminUsagePage = {
  admin_time_zone: string;
  now: string;
  start: string;
  end: string;
  limit: number;
  window?: AdminUsageWindow;
  top_users?: AdminUsageUser[];
  events: AdminUsageEvent[];
  next_before_id?: number;
  prev_after_id?: number;
  cursor_active: boolean;
};

export type AdminUsageTimeSeriesPoint = {
  bucket: string;
  requests: number;
  tokens: number;
  committed_usd: number;
  cache_ratio: number;
  avg_first_token_latency: number;
  tokens_per_second: number;
};

type AdminUsageTimeSeriesResponse = {
  admin_time_zone: string;
  start: string;
  end: string;
  granularity: 'hour' | 'day';
  points: AdminUsageTimeSeriesPoint[];
};

export type UsageEventDetail = {
  event_id: number;
  pricing_breakdown?: UsageEventPricingBreakdown;
};

export type UsageEventPricingBreakdown = {
  model_public_id?: string | null;
  model_found: boolean;
  owned_by?: string | null;
  service_tier?: string | null;
  pricing_kind?: string;

  input_tokens_total: number;
  input_tokens_cache_read: number;
  input_tokens_cache_creation: number;
  input_tokens_cache_creation_5m: number;
  input_tokens_cache_creation_1h: number;
  input_tokens_billable: number;
  output_tokens_total: number;

  input_usd_per_1m: string;
  output_usd_per_1m: string;
  cache_read_usd_per_1m: string;
  cache_creation_5m_usd_per_1m: string;
  cache_creation_1h_usd_per_1m: string;

  input_cost_usd: string;
  output_cost_usd: string;
  cache_read_cost_usd: string;
  cache_creation_cost_usd: string;
  cache_creation_5m_cost_usd: string;
  cache_creation_1h_cost_usd: string;
  base_cost_usd: string;

  tier_multiplier: number;
  channel_multiplier: number;
  final_cost_usd: string;
};

export async function getAdminUsagePage(params: {
  start?: string;
  end?: string;
  all_time?: boolean;
  limit?: number;
  before_id?: number;
  after_id?: number;
  user_id?: number;
  channel_id?: number;
  model?: string;
  index?: string;
  q?: string;
  q_user?: string;
  q_channel?: string;
  q_model?: string;
  summary?: boolean;
}) {
  return getData<APIResponse<AdminUsagePage>>('/api/admin/request', { params });
}

export async function getAdminUsageEventDetail(eventID: number) {
  return getData<APIResponse<UsageEventDetail>>(`/api/admin/request/events/${eventID}/detail`);
}

export async function getAdminUsageTimeSeries(params?: {
  start?: string;
  end?: string;
  all_time?: boolean;
  granularity?: 'hour' | 'day';
}) {
  return getData<APIResponse<AdminUsageTimeSeriesResponse>>('/api/admin/request/timeseries', { params });
}
