import { api } from './client';
import type { APIResponse } from './types';

export type UserToken = {
  id: number;
  name?: string | null;
  status: number;
};

type CreatedToken = {
  token_id: number;
  token: string;
};

type RevealedToken = {
  token_id: number;
  token: string;
};

export async function listUserTokens() {
  const res = await api.get<APIResponse<UserToken[]>>('/api/token');
  return res.data;
}

export async function createUserToken(name?: string) {
  const res = await api.post<APIResponse<CreatedToken>>('/api/token', {
    name: name || undefined,
  });
  return res.data;
}

export async function rotateUserToken(tokenID: number) {
  const res = await api.post<APIResponse<CreatedToken>>(`/api/token/${tokenID}/rotate`);
  return res.data;
}

export async function revealUserToken(tokenID: number) {
  const res = await api.get<APIResponse<RevealedToken>>(`/api/token/${tokenID}/reveal`);
  return res.data;
}

export async function revokeUserToken(tokenID: number) {
  const res = await api.post<APIResponse<void>>(`/api/token/${tokenID}/revoke`);
  return res.data;
}

export async function deleteUserToken(tokenID: number) {
  const res = await api.delete<APIResponse<void>>(`/api/token/${tokenID}`);
  return res.data;
}

export type TokenChannelGroupOption = {
  name: string;
  description?: string | null;
  status: number;
  price_multiplier: string;
};

export type TokenChannelGroupBinding = {
  channel_group_name: string;
  priority: number;
};

export type UserTokenChannelGroups = {
  token_id: number;
  allowed_channel_groups: TokenChannelGroupOption[];
  bindings: TokenChannelGroupBinding[];
  effective_bindings: TokenChannelGroupBinding[];
};

export async function getUserTokenChannelGroups(tokenID: number) {
  const res = await api.get<APIResponse<UserTokenChannelGroups>>(`/api/token/${tokenID}/channel-groups`);
  return res.data;
}

export async function replaceUserTokenChannelGroups(tokenID: number, channelGroups: string[]) {
  const res = await api.put<APIResponse<void>>(`/api/token/${tokenID}/channel-groups`, {
    channel_groups: channelGroups,
  });
  return res.data;
}

export type TokenModelTargetOption = {
  public_id: string;
  group_name: string;
  owned_by?: string | null;
  icon_url?: string | null;
};

export type TokenModelMapping = {
  input_model: string;
  target_model: string;
};

export type UserTokenModelMappings = {
  token_id: number;
  available_target_models: TokenModelTargetOption[];
  mappings: TokenModelMapping[];
};

export async function getUserTokenModelMappings(tokenID: number) {
  const res = await api.get<APIResponse<UserTokenModelMappings>>(`/api/token/${tokenID}/model-mappings`);
  return res.data;
}

export async function replaceUserTokenModelMappings(tokenID: number, mappings: TokenModelMapping[]) {
  const res = await api.put<APIResponse<void>>(`/api/token/${tokenID}/model-mappings`, {
    mappings,
  });
  return res.data;
}
