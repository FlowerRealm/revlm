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
};

export type ChannelUsage = {
  committed_usd: string;
  tokens: number;
  cache_ratio: string;
  avg_first_token_latency: string;
  tokens_per_second: string;
};

type ChannelUsageOverview = {
  requests: number;
  tokens: number;
  committed_usd: string;
  cache_ratio: string;
  avg_first_token_latency: string;
  tokens_per_second: string;
};

export type ChannelRuntime = {
  available: boolean;
  fail_score?: number | null;
  banned_until?: string;
  banned_remaining?: string;
  ban_streak?: number | null;
  banned_active: boolean;
};

export type ChannelAdminItem = Channel & {
  in_use: boolean;
  usage: ChannelUsage;
  runtime: ChannelRuntime;
};

type ChannelsPageResponse = {
  admin_time_zone: string;
  start: string;
  end: string;
  overview: ChannelUsageOverview;
  channels: ChannelAdminItem[];
};

export type ChannelTimeSeriesPoint = {
  bucket: string;
  committed_usd: number;
  tokens: number;
  cache_ratio: number;
  avg_first_token_latency: number;
  tokens_per_second: number;
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
  groups?: string;
  base_url: string;
  key?: string;
  priority?: number;
  price_multiplier?: number;
};

type UpdateChannelRequest = {
  id: number;
  name?: string;
  groups?: string;
  base_url?: string;
  key?: string;
  status?: boolean;
  priority?: number;
  price_multiplier?: number;
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
