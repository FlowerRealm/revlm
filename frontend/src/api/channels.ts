import { deleteData, getData, postData, putData } from './request';
import type { APIResponse } from './types';

export type Channel = {
  id: number;
  type: string;
  name: string;
  groups: string;
  status: boolean;
  priority: number;
  base_url?: string;
  api_key?: string;
  price_multiplier?: number;
  config_json?: Record<string, unknown>;
};

// Money and latency arrive as decimal strings; token counts are protocol-shaped
// and no longer aggregated by the core (ADR 0004).
export type ChannelUsage = {
  usd: string;
  avg_first_token_latency: string;
};

type ChannelUsageOverview = {
  requests: number;
  usd: string;
  avg_first_token_latency: string;
};

export type ChannelRuntime = {
  available: boolean;
  fail_score?: number | null;
  banned_until?: string;
  banned_remaining?: string;
  ban_streak?: number | null;
  banned_active: boolean;
};

export type ChannelItem = Channel & {
  in_use: boolean;
  usage: ChannelUsage;
  runtime: ChannelRuntime;
};

type ChannelsPageResponse = {
  admin_time_zone: string;
  start: string;
  end: string;
  overview: ChannelUsageOverview;
  channels: ChannelItem[];
};

export type ChannelTimeSeriesPoint = {
  bucket: string;
  usd: string;
  avg_first_token_latency: string;
};

type ChannelTimeSeriesResponse = {
  admin_time_zone: string;
  channel_id: number;
  start: string;
  end: string;
  granularity: 'hour' | 'day';
  points: ChannelTimeSeriesPoint[];
};

type CreateChannelRequest = {
  type: string;
  name: string;
  status?: boolean;
  groups?: string;
  base_url: string;
  key?: string;
  priority?: number;
  price_multiplier?: number;
  config_json?: Record<string, unknown>;
};

type UpdateChannelRequest = {
  id: number;
  type?: string;
  name?: string;
  groups?: string;
  base_url?: string;
  key?: string;
  status?: boolean;
  priority?: number;
  price_multiplier?: number;
  config_json?: Record<string, unknown>;
};

export async function getChannelsPage(params?: { start?: string; end?: string; all_time?: boolean }) {
  return getData<APIResponse<ChannelsPageResponse>>('/api/channel/page', { params });
}

export async function getChannelTimeSeries(
  channelID: number,
  params?: { start?: string; end?: string; all_time?: boolean; granularity?: 'hour' | 'day' }
) {
  return getData<APIResponse<ChannelTimeSeriesResponse>>(`/api/channel/${channelID}/timeseries`, { params });
}

export async function createChannel(req: CreateChannelRequest) {
  return postData<APIResponse<{ id: number }>>('/api/channel', req);
}

export async function updateChannel(req: UpdateChannelRequest) {
  return putData<APIResponse<void>>('/api/channel', req);
}

export async function deleteChannel(channelID: number) {
  return deleteData<APIResponse<void>>(`/api/channel/${channelID}`);
}
