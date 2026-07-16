import type { UserToken } from '../../api/tokens';
import type { UsageEvent } from '../../api/usage';
import { formatUSDPlain } from '../../format/money';

export type TopUserView = {
  user_id: number;
  email: string;
  role: string;
  status: number;
  usd: string;
};

export function formatLocalDate(iso: string): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return '';
  const yyyy = d.getFullYear();
  const mm = String(d.getMonth() + 1).padStart(2, '0');
  const dd = String(d.getDate()).padStart(2, '0');
  return `${yyyy}-${mm}-${dd}`;
}

export function formatLocalDateTime(iso: string): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return iso;
  const yyyy = d.getFullYear();
  const mm = String(d.getMonth() + 1).padStart(2, '0');
  const dd = String(d.getDate()).padStart(2, '0');
  const hh = String(d.getHours()).padStart(2, '0');
  const mi = String(d.getMinutes()).padStart(2, '0');
  const ss = String(d.getSeconds()).padStart(2, '0');
  return `${yyyy}-${mm}-${dd} ${hh}:${mi}:${ss}`;
}

export function formatLocalDateTimeMinute(iso: string): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return iso;
  const yyyy = d.getFullYear();
  const mm = String(d.getMonth() + 1).padStart(2, '0');
  const dd = String(d.getDate()).padStart(2, '0');
  const hh = String(d.getHours()).padStart(2, '0');
  const mi = String(d.getMinutes()).padStart(2, '0');
  return `${yyyy}-${mm}-${dd} ${hh}:${mi}`;
}

export function cacheHitRate(ratio: number): string {
  if (!Number.isFinite(ratio)) return '0.0%';
  return `${(ratio * 100).toFixed(1)}%`;
}

export function tokenNameFromMap(tokenByID: Record<number, UserToken>, tokenID: number): string {
  const tok = tokenByID[tokenID];
  const name = (tok?.name || '').toString().trim();
  if (name) return name;
  if (tok?.id) return `Token #${tok.id}`;
  return '-';
}

export function formatDecimalPlain(raw: string | number | null | undefined): string {
  let s = (raw ?? '').toString().trim();
  if (!s) return '0';
  if (s.startsWith('+')) s = s.slice(1).trim();
  if (s.startsWith('$')) s = s.slice(1).trim();
  if (!s) return '0';
  if (s.includes('.')) {
    s = s.replace(/0+$/, '').replace(/\.$/, '');
  }
  if (s === '-0' || s === '') return '0';
  return s;
}

export function formatUSD(raw: string): string {
  const s = formatDecimalPlain(raw);
  if (s.startsWith('-')) return `-$${s.slice(1)}`;
  return `$${s}`;
}

export function normalizeServiceTier(raw?: string | null): string {
  const tier = (raw || '').trim().toLowerCase();
  if (tier === 'fast' || tier === 'priority') return 'priority';
  return tier;
}

export function serviceTierBadgeLabel(raw?: string | null): string {
  const tier = normalizeServiceTier(raw);
  return tier ? tier.toUpperCase() : '';
}

export function serviceTierText(raw?: string | null): string {
  const tier = normalizeServiceTier(raw);
  return tier || '-';
}

export const priorityServiceTierBadgeClassName =
  'badge bg-success-subtle text-success border border-success-subtle rounded-pill px-2 scale-90 mt-1';

export function tokensPerSecond(ev: UsageEvent): string {
  const outTokens = ev.output_tokens ?? 0;
  const latencyMS = ev.latency_ms ?? 0;
  if (!Number.isFinite(outTokens) || outTokens <= 0) return '-';
  if (!Number.isFinite(latencyMS) || latencyMS <= 0) return '-';
  return ((outTokens * 1000) / latencyMS).toFixed(2);
}

export function errorText(errClass?: string | null, errMessage?: string | null): string {
  const cls = (errClass || '').toString().trim();
  const msg = (errMessage || '').toString().trim();
  let out = '';
  if (cls) out = cls;
  if (msg) out = out ? `${out} (${msg})` : msg;
  return out;
}

export function costLabel(ev: UsageEvent): string {
  return formatUSDPlain(ev.cost_usd);
}
