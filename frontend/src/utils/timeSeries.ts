export function normalizeDailyBucketLabel(bucket: string): string {
  return (bucket || '').trim().slice(0, 10);
}

function parseDateOnlyUTC(raw: string): Date | null {
  const v = (raw || '').trim();
  if (!/^\d{4}-\d{2}-\d{2}$/.test(v)) return null;
  return new Date(`${v}T00:00:00Z`);
}

function formatDateOnlyUTC(d: Date): string {
  return d.toISOString().slice(0, 10);
}

export function fillDailyBuckets<T extends { bucket: string }>(
  points: T[],
  start: string,
  end: string,
  zeroPoint: (bucket: string) => T
): T[] {
  const normalized = (points || []).map((point) => ({
    ...point,
    bucket: normalizeDailyBucketLabel(point.bucket),
  }));
  const startDate = parseDateOnlyUTC(start);
  const endDate = parseDateOnlyUTC(end);
  if (!startDate || !endDate || startDate.getTime() > endDate.getTime()) {
    return normalized;
  }

  const byDay = new Map<string, T>();
  for (const point of normalized) {
    const key = normalizeDailyBucketLabel(point.bucket);
    if (!key) continue;
    byDay.set(key, point);
  }

  const out: T[] = [];
  for (
    let cursor = new Date(startDate);
    cursor.getTime() <= endDate.getTime();
    cursor.setUTCDate(cursor.getUTCDate() + 1)
  ) {
    const day = formatDateOnlyUTC(cursor);
    out.push(byDay.get(day) || zeroPoint(day));
  }
  return out;
}
