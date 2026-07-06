import { api } from '../client';
import type { APIResponse } from '../types';

export type AdminDashboardStats = {
  users_count: number;
  channels_count: number;
  endpoints_count: number;
  requests_today: number;
  tokens_today: number;
  input_tokens_today: number;
  output_tokens_today: number;
  cost_today: string;
};

export type AdminDashboard = {
  admin_time_zone: string;
  stats: AdminDashboardStats;
};

export async function getAdminDashboard() {
  const res = await api.get<APIResponse<AdminDashboard>>('/api/admin/dashboard');
  return res.data;
}
