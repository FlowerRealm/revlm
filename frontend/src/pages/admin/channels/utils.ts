export function parseGroupsCSV(raw: string): string[] {
  const s = raw.trim();
  if (!s) return [];
  const uniq = new Set<string>();
  for (const part of s.split(',')) {
    const v = part.trim();
    if (v) uniq.add(v);
  }
  return Array.from(uniq);
}

export function toggleGroupsCSV(raw: string, name: string, checked: boolean): string {
  const set = new Set(parseGroupsCSV(raw));
  if (checked) set.add(name);
  else set.delete(name);
  return Array.from(set).join(',');
}

export function validateJSON(raw: string, kind: 'object' | 'array'): string {
  const s = (raw || '').trim();
  if (!s) return '';
  try {
    const v = JSON.parse(s) as unknown;
    if (kind === 'array') {
      if (!Array.isArray(v)) return 'JSON 必须为数组';
      return '';
    }
    if (!v || typeof v !== 'object' || Array.isArray(v)) {
      return 'JSON 必须为对象';
    }
    return '';
  } catch {
    return 'JSON 不合法';
  }
}
