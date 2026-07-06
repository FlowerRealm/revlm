export type UsageAdminDetailField =
  | 'committed_usd'
  | 'requests'
  | 'tokens'
  | 'cache_ratio'
  | 'avg_first_token_latency'
  | 'tokens_per_second';

export type UsageAdminDetailGranularity = 'hour' | 'day';

export type UsageAdminQueryState = {
  start: string;
  end: string;
  allTime: boolean;
  limit: number;
  beforeID?: number;
  afterID?: number;
  filterUser: string;
  filterUserID?: number;
  filterChannel: string;
  filterChannelID?: number;
  filterModel: string;
  filterModelExact?: string;
};

export type UsageAdminRefreshOverride = Partial<{
  start: string;
  end: string;
  allTime: boolean;
  filterUser: string;
  filterUserID: number | undefined;
  filterChannel: string;
  filterChannelID: number | undefined;
  filterModel: string;
  filterModelExact: string | undefined;
}>;

export type UsageAdminCursorOverride = {
  beforeID?: number;
  afterID?: number;
};

export const usageAdminFieldOptions: Array<{
  value: UsageAdminDetailField;
  label: string;
}> = [
  { value: 'committed_usd', label: '消耗 (USD)' },
  { value: 'requests', label: '请求数' },
  { value: 'tokens', label: 'Token' },
  { value: 'cache_ratio', label: '缓存率 (%)' },
  { value: 'avg_first_token_latency', label: '首字延迟 (s)' },
  { value: 'tokens_per_second', label: 'Tokens/s' },
];

export const usageAdminGranularityOptions: Array<{
  value: UsageAdminDetailGranularity;
  label: string;
}> = [
  { value: 'hour', label: '按小时' },
  { value: 'day', label: '按天' },
];

export function isAllTimeRange(allTime: boolean, start: string, end: string) {
  return allTime && !start.trim() && !end.trim();
}

export function buildAdminUsagePageParams(
  state: UsageAdminQueryState,
  override?: UsageAdminRefreshOverride,
  keepCursor = false,
  cursor?: UsageAdminCursorOverride
) {
  const query = resolveQueryInput(state, override);
  const allTimeActive = isAllTimeRange(query.allTimeValue, query.startValue, query.endValue);
  const params = buildFilterParams(state.limit, query);

  applyRangeParams(params, allTimeActive, query.startValue, query.endValue);
  applyCursorParams(params, state, keepCursor, cursor);

  return {
    params,
    allTimeActive,
    startValue: query.startValue,
    endValue: query.endValue,
  };
}

export function badgeForState(cls: string): string {
  const stateClass = (cls || '').trim();
  if (stateClass) return `badge rounded-pill ${stateClass}`;
  return 'badge rounded-pill bg-light text-secondary border';
}

export function formatDecimalPlain(raw: string): string {
  let value = (raw || '').toString().trim();
  if (!value) return '0';
  if (value.startsWith('+')) value = value.slice(1).trim();
  if (value.startsWith('$')) value = value.slice(1).trim();
  if (!value) return '0';
  if (value.includes('.')) {
    value = value.replace(/0+$/, '').replace(/\.$/, '');
  }
  if (value === '-0' || value === '') return '0';
  return value;
}

export function formatUSD(raw: string): string {
  const normalized = formatDecimalPlain(raw);
  if (normalized.startsWith('-')) return `-$${normalized.slice(1)}`;
  return `$${normalized}`;
}

function resolveQueryInput(state: UsageAdminQueryState, override?: UsageAdminRefreshOverride) {
  return {
    startValue: (override?.start ?? state.start).trim(),
    endValue: (override?.end ?? state.end).trim(),
    allTimeValue: !!(override?.allTime ?? state.allTime),
    qUser: (override?.filterUser ?? state.filterUser).trim(),
    qUserID: override?.filterUserID ?? state.filterUserID,
    qChannel: (override?.filterChannel ?? state.filterChannel).trim(),
    qChannelID: override?.filterChannelID ?? state.filterChannelID,
    qModel: (override?.filterModel ?? state.filterModel).trim(),
    qModelExact: override?.filterModelExact ?? state.filterModelExact,
  };
}

function buildFilterParams(limit: number, query: ReturnType<typeof resolveQueryInput>) {
  return {
    limit,
    index: buildIndex(query),
    user_id: typeof query.qUserID === 'number' && query.qUserID > 0 ? query.qUserID : undefined,
    upstream_channel_id: typeof query.qChannelID === 'number' && query.qChannelID > 0 ? query.qChannelID : undefined,
    model: query.qModelExact || undefined,
    q_user: !query.qUserID ? query.qUser || undefined : undefined,
    q_channel: !query.qChannelID ? query.qChannel || undefined : undefined,
    q_model: !query.qModelExact ? query.qModel || undefined : undefined,
  } as {
    start?: string;
    end?: string;
    all_time?: boolean;
    limit: number;
    before_id?: number;
    after_id?: number;
    summary?: boolean;
    user_id?: number;
    upstream_channel_id?: number;
    model?: string;
    index?: string;
    q_user?: string;
    q_channel?: string;
    q_model?: string;
  };
}

function buildIndex(query: ReturnType<typeof resolveQueryInput>) {
  const indexParts = [
    !query.qUserID && query.qUser ? 'user' : '',
    !query.qChannelID && query.qChannel ? 'channel' : '',
    !query.qModelExact && query.qModel ? 'model' : '',
  ].filter(Boolean);

  return indexParts.length ? indexParts.join(',') : undefined;
}

function applyRangeParams(
  params: ReturnType<typeof buildFilterParams>,
  allTimeActive: boolean,
  startValue: string,
  endValue: string
) {
  if (allTimeActive) {
    params.all_time = true;
    return;
  }

  params.start = startValue || undefined;
  params.end = endValue || undefined;
}

function applyCursorParams(
  params: ReturnType<typeof buildFilterParams>,
  state: UsageAdminQueryState,
  keepCursor: boolean,
  cursor?: UsageAdminCursorOverride
) {
  if (!keepCursor) return;

  const cursorBeforeID = cursor && 'beforeID' in cursor ? cursor.beforeID : state.beforeID;
  const cursorAfterID = cursor && 'afterID' in cursor ? cursor.afterID : state.afterID;

  if (cursorBeforeID) params.before_id = cursorBeforeID;
  if (cursorAfterID) params.after_id = cursorAfterID;
  if (cursorBeforeID || cursorAfterID) params.summary = false;
}
