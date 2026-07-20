import { getData } from '../request';
import type { APIResponse } from '../types';
import type { UsageEventDetail } from '../usageDetail';

export type { UsageEventDetail, UsageEventPricingBreakdown } from '../usageDetail';

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
  usd: string;
};

export type AdminUsageUser = {
  user_id: number;
  email: string;
  role: string;
  status: number;
  usd: string;
};

export type AdminUsageEvent = {
  id: number;
  time: string;
  user_id: number;
  user_email: string;
  endpoint?: string | null;
  method?: string | null;
  model: string;
  model_name?: string | null;
  status_code: number;
  latency_ms: number;
  first_token_latency_ms: number;
  tokens_per_second: string;
  input_tokens: number;
  output_tokens: number;
  cached_tokens: number;
  tier_multiplier?: number;
  channel_multiplier?: number;
  cost_usd: string;
  service_tier?: string | null;
  is_stream: boolean;
  channel_id: number;
  upstream_channel_name: string;
  request_id: string;
  response_id?: string | null;
  error: string;
  error_class?: string | null;
  error_message?: string | null;
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
  usd: number;
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
