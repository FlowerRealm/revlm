export function parseMilliseconds(input: unknown): number | null {
  if (input === null || input === undefined) return null;

  if (typeof input === 'number') {
    if (!Number.isFinite(input) || input <= 0) return null;
    return input;
  }

  if (typeof input === 'string') {
    let s = input.trim();
    if (!s || s === '-') return null;

    s = s.replace(/\s*ms\s*$/i, '').trim();
    if (!s || s === '-') return null;

    const n = Number(s);
    if (!Number.isFinite(n) || n <= 0) return null;
    return n;
  }

  return null;
}

export function formatSecondsFromMilliseconds(input: unknown, digits = 2): string {
  const ms = parseMilliseconds(input);
  if (ms === null) return '-';
  return `${(ms / 1000).toFixed(digits)}s`;
}

export function formatLatencyPairSeconds(totalMs: unknown, firstMs: unknown, digits = 2): string {
  const total = parseMilliseconds(totalMs);
  if (total === null) return '-';

  const fmt = (ms: number) => `${(ms / 1000).toFixed(digits)}s`;

  const first = parseMilliseconds(firstMs);
  if (first === null) return fmt(total);

  return `${fmt(total)} / ${fmt(first)}`;
}
