export type DateInputRangePreset = {
  label: string;
  start: string;
  end: string;
};

export function formatDateInputLocal(date: Date): string {
  const yyyy = date.getFullYear();
  const mm = String(date.getMonth() + 1).padStart(2, '0');
  const dd = String(date.getDate()).padStart(2, '0');
  return `${yyyy}-${mm}-${dd}`;
}

export function todayDateInputLocal(now = new Date()): string {
  return formatDateInputLocal(now);
}

function shiftLocalDays(date: Date, days: number): Date {
  return new Date(date.getFullYear(), date.getMonth(), date.getDate() + days);
}

export function buildDateInputRangePresets(now = new Date()): DateInputRangePreset[] {
  const currentMonthStart = new Date(now.getFullYear(), now.getMonth(), 1);
  const previousMonthStart = new Date(now.getFullYear(), now.getMonth() - 1, 1);
  const previousMonthEnd = new Date(now.getFullYear(), now.getMonth(), 0);

  return [
    {
      label: '今天',
      start: formatDateInputLocal(now),
      end: formatDateInputLocal(now),
    },
    {
      label: '昨天',
      start: formatDateInputLocal(shiftLocalDays(now, -1)),
      end: formatDateInputLocal(shiftLocalDays(now, -1)),
    },
    {
      label: '过去 7 天',
      start: formatDateInputLocal(shiftLocalDays(now, -7)),
      end: formatDateInputLocal(now),
    },
    {
      label: '本月',
      start: formatDateInputLocal(currentMonthStart),
      end: formatDateInputLocal(now),
    },
    {
      label: '上个月',
      start: formatDateInputLocal(previousMonthStart),
      end: formatDateInputLocal(previousMonthEnd),
    },
    {
      label: '全部时间',
      start: '',
      end: '',
    },
  ];
}
