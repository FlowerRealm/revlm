import { deleteData, getData, postData } from './request';
import type { APIResponse } from './types';

export type PluginMigration = {
  id: string;
  applied_at: string;
};

export type PluginInstallation = {
  id: string;
  name: string;
  version: string;
  core_abi: string;
  status: string;
  path: string;
  target: { os: string; arch: string };
  enabled: boolean;
  system_plugin: boolean;
  error: string;
  migrations: PluginMigration[];
};

export async function listPlugins() {
  return getData<APIResponse<PluginInstallation[]>>('/api/admin/plugins');
}

export async function uploadPlugin(file: File) {
  return postData<APIResponse<void>, File>('/api/admin/plugins/upload', file, {
    headers: {
      'Content-Type': 'application/octet-stream',
      'X-Plugin-Filename': file.name,
    },
  });
}

export async function setPluginEnabled(pluginID: string, enabled: boolean) {
  return postData<APIResponse<void>>(
    `/api/admin/plugins/${encodeURIComponent(pluginID)}/${enabled ? 'enable' : 'disable'}`
  );
}

export async function uninstallPlugin(pluginID: string) {
  return deleteData<APIResponse<void>>(`/api/admin/plugins/${encodeURIComponent(pluginID)}`);
}
