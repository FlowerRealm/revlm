import { api } from './client';
import type { APIResponse } from './types';

export type UserToken = {
  id: number;
  name?: string | null;
  status: number;
  channel_group_id?: number;
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
  id: number;
  name: string;
  description?: string;
  status: boolean;
  price_multiplier: number;
};

export type UserTokenChannelGroup = {
  token_id: number;
  channel_group_id: number;
  allowed_channel_groups: TokenChannelGroupOption[];
};

export async function getUserTokenChannel(tokenID: number) {
  const res = await api.get<APIResponse<UserTokenChannelGroup>>(`/api/token/${tokenID}/channel`);
  return res.data;
}

export async function setUserTokenChannel(tokenID: number, channelGroupID: number) {
  const res = await api.put<APIResponse<void>>(`/api/token/${tokenID}/channel`, {
    channel_group_id: channelGroupID,
  });
  return res.data;
}
