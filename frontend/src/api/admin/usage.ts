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
  account: string;
  status_code: string;
  latency_ms: string;
  first_token_latency_ms: string;
  tokens_per_second: string;
  input_tokens: string;
  output_tokens: string;
  cached_tokens: string;
  request_bytes: string;
  response_bytes: string;
  cost_usd: string;
  state_label: string;
  state_badge_class: string;
  requested_service_tier?: string;
  service_tier?: string;
  service_tier_downgraded: boolean;
  service_tier_downgrade_reason?: string;
  is_stream: boolean;
  upstream_channel_id: string;
  upstream_channel_name: string;
  request_id: string;
  error: string;
  error_class: string;
  error_message: string;
  model_mismatch: boolean;
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

export async function getAdminUsagePage(params: {
  start?: string;
  end?: string;
  all_time?: boolean;
  limit?: number;
  before_id?: number;
  after_id?: number;
  user_id?: number;
  upstream_channel_id?: number;
  model?: string;
  index?: string;
  q?: string;
  q_user?: string;
  q_channel?: string;
  q_model?: string;
  summary?: boolean;
}) {
  return getData<APIResponse<AdminUsagePage>>('/api/admin/usage', { params });
}

export type AdminUsageUserSuggest = {
  id: number;
  email: string;
  username: string;
};

export async function getAdminUsageUserSuggest(q: string, limit = 20) {
  const params = { q, limit };
  return getData<APIResponse<AdminUsageUserSuggest[]>>('/api/admin/usage/users/suggest', { params });
}

export type AdminUsageChannelSuggest = {
  id: number;
  name: string;
  type: string;
};

export async function getAdminUsageChannelSuggest(params: {
  q: string;
  limit?: number;
  start?: string;
  end?: string;
  all_time?: boolean;
}) {
  return getData<APIResponse<AdminUsageChannelSuggest[]>>('/api/admin/usage/channels/suggest', { params });
}

export type AdminUsageModelSuggest = { model: string };

export async function getAdminUsageModelSuggest(params: {
  q: string;
  limit?: number;
  start?: string;
  end?: string;
  all_time?: boolean;
}) {
  return getData<APIResponse<AdminUsageModelSuggest[]>>('/api/admin/usage/models/suggest', { params });
}

export async function getAdminUsageEventDetail(eventID: number) {
  return getData<APIResponse<UsageEventDetail>>(`/api/admin/usage/events/${eventID}/detail`);
}

export async function getAdminUsageTimeSeries(params?: {
  start?: string;
  end?: string;
  all_time?: boolean;
  granularity?: 'hour' | 'day';
}) {
  return getData<APIResponse<AdminUsageTimeSeriesResponse>>('/api/admin/usage/timeseries', { params });
}
