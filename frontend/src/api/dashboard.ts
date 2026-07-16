import { api } from './client';
import type { APIResponse } from './types';
import { browserTimeZone } from './timezone';
import type { UsageTimeSeriesPoint } from './usage';

type DashboardModelUsage = {
  model: string;
  icon_url?: string;
  color: string;
  requests: number;
  tokens: number;
  usd: string;
};

type DashboardTimeSeriesUsage = UsageTimeSeriesPoint;

export type DashboardCharts = {
  model_stats: DashboardModelUsage[];
  time_series_stats: DashboardTimeSeriesUsage[];
};

export type DashboardData = {
  today_usage_usd: string;
  today_since: string;
  today_until: string;
  today_requests: number;
  today_tokens: number;
  today_rpm: string;
  today_tpm: string;
  charts: DashboardCharts;
};

export async function getDashboard() {
  const res = await api.get<APIResponse<DashboardData>>('/api/dashboard', {
    params: {
      tz: browserTimeZone(),
    },
  });
  return res.data;
}
