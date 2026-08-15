import { api } from './client';
import type { APIResponse } from './types';

/**
 * A model catalogue entry exactly as the plugin that owns it registered it.
 *
 * Only `id` and `name` are shared; `pricing` is the plugin's own JSON and its
 * keys mean whatever that protocol says they mean, so the console renders them
 * without interpreting them (CONTEXT 模型 / 模型目录).
 */
export type PluginModel = {
  id: string;
  name?: string;
  pricing?: Record<string, unknown>;
};

export async function listUserModelsDetail() {
  const res = await api.get<APIResponse<PluginModel[]>>('/api/user/models/detail');
  return res.data;
}
