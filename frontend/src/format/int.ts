function groupDigitsComma(digits: string): string {
  let out = '';
  for (let i = digits.length; i > 0; i -= 3) {
    const start = Math.max(0, i - 3);
    const chunk = digits.slice(start, i);
    out = out ? `${chunk},${out}` : chunk;
  }
  return out || '0';
}

function formatIntStringComma(raw: string): string {
  let sign = '';
  let digits = raw;
  if (digits.startsWith('-')) {
    sign = '-';
    digits = digits.slice(1);
  }
  return sign + groupDigitsComma(digits);
}

export function formatIntComma(v: number | bigint | string | null | undefined): string {
  if (v == null) return '-';

  if (typeof v === 'number') {
    if (!Number.isFinite(v)) return '-';
    if (!Number.isInteger(v)) return String(v);
    return formatIntStringComma(v.toString());
  }

  if (typeof v === 'bigint') {
    return formatIntStringComma(v.toString());
  }

  const s = String(v).trim();
  if (!s) return '-';
  if (!/^-?\d+$/.test(s)) return s;
  return formatIntStringComma(s);
}
