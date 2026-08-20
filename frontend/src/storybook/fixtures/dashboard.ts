import type { DashboardData } from '../../api/dashboard';
import { usageTimeSeries } from './usage';

export const dashboardData: DashboardData = {
  today_usage_usd: '18.42',
  today_since: '2026-08-19 00:00:00',
  today_until: '2026-08-19 09:12:00',
  today_requests: 812,
  today_rpm: '1.4',
  charts: {
    model_stats: [
      { model: 'claude-opus-5', color: '#326c52', requests: 420, usd: '11.20' },
      { model: 'claude-sonnet-5', color: '#5b8f74', requests: 392, usd: '7.22' },
    ],
    time_series_stats: usageTimeSeries,
  },
};

export const emptyDashboardData: DashboardData = {
  today_usage_usd: '0',
  today_since: '2026-08-19 00:00:00',
  today_until: '2026-08-19 00:00:00',
  today_requests: 0,
  today_rpm: '0',
  charts: { model_stats: [], time_series_stats: [] },
};
