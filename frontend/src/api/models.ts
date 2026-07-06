import { api } from './client';
import type { APIResponse } from './types';

export type UserManagedModel = {
  id: number;
  public_id: string;
  group_name: string;
  owned_by: string;
  input_usd_per_1m: string;
  output_usd_per_1m: string;
  cache_read_input_usd_per_1m: string;
  cache_creation_input_usd_per_1m: string;
  cache_creation_1h_input_usd_per_1m: string;
  status: number;
  icon_url?: string | null;
};

export async function listUserModelsDetail() {
  const res = await api.get<APIResponse<UserManagedModel[]>>('/api/user/models/detail');
  return res.data;
}
