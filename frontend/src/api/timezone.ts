export function browserTimeZone(): string | undefined {
  try {
    const tz = Intl.DateTimeFormat().resolvedOptions().timeZone;
    if (!tz || !tz.trim()) return undefined;
    return tz.trim();
  } catch {
    return undefined;
  }
}
