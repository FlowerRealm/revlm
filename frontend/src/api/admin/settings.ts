import { api } from '../client';
import type { APIResponse } from '../types';

export type AdminSettings = {
  site_base_url: string;
  site_base_url_override: boolean;
  site_base_url_effective: string;
  site_base_url_invalid: boolean;

  billing_paygo_price_multiplier: number;
  billing_paygo_price_multiplier_override: boolean;
};

export type UpdateAdminSettingsRequest = {
  site_base_url: string;
  /** null clears the DB override and falls back to default 1.0 */
  billing_paygo_price_multiplier: number | null;
};

export async function getAdminSettings() {
  const res = await api.get<APIResponse<AdminSettings>>('/api/admin/settings');
  return res.data;
}

export async function updateAdminSettings(req: UpdateAdminSettingsRequest) {
  const res = await api.put<APIResponse<void>>('/api/admin/settings', req);
  return res.data;
}
