import { api } from '../client';
import type { APIResponse } from '../types';

export type AdminUser = {
  id: number;
  email: string;
  username: string;
  role: string;
  status: number;
  balance_usd: number;
};

export async function listAdminUsers() {
  const res = await api.get<APIResponse<AdminUser[]>>('/api/admin/users');
  return res.data;
}

type CreateAdminUserRequest = {
  email: string;
  username: string;
  password: string;
  role?: string;
};

export async function createAdminUser(req: CreateAdminUserRequest) {
  const res = await api.post<APIResponse<{ id: number }>>('/api/admin/users', req);
  return res.data;
}

type UpdateAdminUserRequest = {
  email?: string;
  status?: number;
  role?: string;
};

export async function updateAdminUser(userID: number, req: UpdateAdminUserRequest) {
  const res = await api.put<APIResponse<void>>(`/api/admin/users/${userID}`, req);
  return res.data;
}

export async function resetAdminUserPassword(userID: number, password: string) {
  const res = await api.post<APIResponse<void>>(`/api/admin/users/${userID}/password`, { password });
  return res.data;
}

export async function addAdminUserBalance(userID: number, amountUSD: string) {
  const res = await api.post<APIResponse<{ balance_usd: number }>>(`/api/admin/users/${userID}/balance`, {
    amount_usd: amountUSD,
  });
  return res.data;
}

export async function deleteAdminUser(userID: number) {
  const res = await api.delete<APIResponse<void>>(`/api/admin/users/${userID}`);
  return res.data;
}
