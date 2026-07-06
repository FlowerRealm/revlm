function normalizeDecimalString(raw: string): { sign: string; value: string } {
  let s = (raw || '').toString().trim();
  if (!s) return { sign: '', value: '0' };

  // Strip optional "$" prefix.
  if (s.startsWith('$')) {
    s = s.slice(1).trim();
  }

  let sign = '';
  if (s.startsWith('-')) {
    sign = '-';
    s = s.slice(1).trim();
  } else if (s.startsWith('+')) {
    s = s.slice(1).trim();
  }

  if (!s) return { sign: '', value: '0' };
  return { sign, value: s };
}

function trimTrailingZeros(raw: string): string {
  let s = raw;
  if (s.includes('.')) {
    s = s.replace(/0+$/, '');
    s = s.replace(/\.$/, '');
  }
  return s || '0';
}

export function formatUSDPlain(v: string | number | null | undefined): string {
  const { sign, value } = normalizeDecimalString(v == null ? '' : String(v));
  const cleaned = trimTrailingZeros(value);
  if (cleaned === '0') return '0';
  return `${sign}${cleaned}`;
}
