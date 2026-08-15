import { api } from './client';
import type { APIResponse } from './types';
import { browserTimeZone } from './timezone';

// Requests, spend and latency only. Token counts are protocol-shaped and live
// inside usage_details, which the core never parses (ADR 0004), so no endpoint
// aggregates them any more.
export type UsageWindow = {
  window: string;
  since: string;
  until: string;
  requests: number;
  rpm: number;
  first_token_samples: number;
  avg_first_token_latency: number;
  usd: string;
};

type UsageWindowsResponse = {
  time_zone?: string;
  now: string;
  windows: UsageWindow[];
};

// The raw protocol usage payload. Its shape belongs to whichever plugin served
// the request, so the core hands it through untouched and the UI treats it as
// opaque JSON rather than a set of known fields.
export type UsageDetails = Record<string, unknown>;

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
  channel_group_multiplier?: number;
  usage_details?: UsageDetails | null;
  cost_usd: string;
  status_code: number;
  latency_ms: number;
  first_token_latency_ms?: number;
  error_message?: string | null;
};

type UsageEventsResponse = {
  events: UsageEvent[];
  next_before_id?: number | null;
};

export type UsageTimeSeriesPoint = {
  bucket: string;
  requests: number;
  usd: number;
  avg_first_token_latency: number;
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
  usage_details?: UsageDetails | null;
};

export async function getUsageEventDetail(eventID: number, tokenID?: number) {
  const res = await api.get<APIResponse<UsageEventDetail>>(`/api/request/events/${eventID}/detail`, {
    params: {
      token_id: tokenID || undefined,
    },
  });
  return res.data;
}
