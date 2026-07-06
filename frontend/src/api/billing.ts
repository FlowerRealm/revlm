import { api } from './client';
import type { APIResponse } from './types';

export type BillingBalanceResponse = {
  balance_usd: string;
};

export async function getBalance() {
  const res = await api.get<APIResponse<BillingBalanceResponse>>('/api/billing/balance');
  return res.data;
}
