import { useEffect, useMemo, useRef, useState } from 'react';

import { listUserTokens, type UserToken } from '../api/tokens';
import {
  getUsageEventDetail,
  getUsageEvents,
  getUsageWindows,
  type UsageEvent,
  type UsageEventDetail,
  type UsageWindow,
} from '../api/usage';
import { useAuth } from '../auth/AuthContext';
import { DateRangePicker, SelectPicker } from '../components/DateRangePicker';
import { SegmentedFrame } from '../components/SegmentedFrame';
import {
  UsageAdvancedFiltersDropdown,
  type UsageAdvancedFiltersDropdownHandle,
} from '../components/UsageAdvancedFiltersDropdown';
import { UsageEventsCard } from './usage/UsageEventsCard';
import { UsageSummaryCard } from './usage/UsageSummaryCard';
import { formatLocalDate, formatLocalDateTimeMinute } from './usage/usageUtils';
import { todayDateInputLocal } from '../utils/dateInput';

export function UsagePage() {
  const { user } = useAuth();

  const [data, setData] = useState<UsageWindow | null>(null);
  const [events, setEvents] = useState<UsageEvent[]>([]);
  const [loading, setLoading] = useState(true);
  const [err, setErr] = useState('');

  const [tokenByID, setTokenByID] = useState<Record<number, UserToken>>({});

  const [start, setStart] = useState(() => todayDateInputLocal());
  const [end, setEnd] = useState(() => todayDateInputLocal());
  const [allTime, setAllTime] = useState(false);
  const [limit, setLimit] = useState(50);
  const [filterKey, setFilterKey] = useState('');
  const [filterModel, setFilterModel] = useState('');
  const advRef = useRef<UsageAdvancedFiltersDropdownHandle | null>(null);

  const [nextBeforeID, setNextBeforeID] = useState<number | null>(null);
  const [beforeStack, setBeforeStack] = useState<number[]>([]);

  const [expandedID, setExpandedID] = useState<number | null>(null);
  const [detailByEventID, setDetailByEventID] = useState<Record<number, UsageEventDetail>>({});
  const [detailLoadingID, setDetailLoadingID] = useState<number | null>(null);

  const canPrev = beforeStack.length > 0;
  const canNext = useMemo(() => !!nextBeforeID && events.length === limit, [events.length, limit, nextBeforeID]);

  async function refresh(
    currentBeforeID?: number,
    override?: { start?: string; end?: string; allTime?: boolean; filterKey?: string; filterModel?: string }
  ) {
    setErr('');
    setLoading(true);
    try {
      const startValue = (override?.start ?? start).trim();
      const endValue = (override?.end ?? end).trim();
      const allTimeValue = !!(override?.allTime ?? allTime);
      const allTimeActive = allTimeValue && !startValue && !endValue;
      const indexParts: string[] = [];
      const q_key = (override?.filterKey ?? filterKey).trim();
      const q_model = (override?.filterModel ?? filterModel).trim();
      if (q_key) indexParts.push('key');
      if (q_model) indexParts.push('model');
      const index = indexParts.length ? indexParts.join(',') : undefined;
      const [w, e] = await Promise.all([
        getUsageWindows(startValue || undefined, endValue || undefined, undefined, allTimeActive),
        getUsageEvents({
          limit,
          before_id: currentBeforeID,
          start: allTimeActive ? undefined : startValue || undefined,
          end: allTimeActive ? undefined : endValue || undefined,
          index,
          q_key: q_key || undefined,
          q_model: q_model || undefined,
        }),
      ]);
      if (!w.success) throw new Error(w.message || '加载失败');
      if (!e.success) throw new Error(e.message || '加载失败');

      const window0 = w.data?.windows?.[0] ?? null;
      setData(window0);
      setEvents(e.data?.events || []);
      setNextBeforeID(e.data?.next_before_id ?? null);

      if (window0 && !allTimeActive) {
        const day0 = formatLocalDate(String(window0.since));
        if (!startValue && day0) setStart(day0);
        if (!endValue && (startValue || day0)) setEnd(endValue || startValue || day0);
      }
    } catch (e) {
      setErr(e instanceof Error ? e.message : '加载失败');
      setData(null);
      setEvents([]);
      setNextBeforeID(null);
    } finally {
      setLoading(false);
    }
  }

  async function loadDetail(eventID: number) {
    if (detailByEventID[eventID]) return;
    setDetailLoadingID(eventID);
    try {
      const res = await getUsageEventDetail(eventID);
      if (!res.success) throw new Error(res.message || '加载详情失败');
      const d = res.data;
      if (d) {
        setDetailByEventID((prev) => ({ ...prev, [eventID]: d }));
      }
    } catch (e) {
      setErr(e instanceof Error ? e.message : '加载详情失败');
    } finally {
      setDetailLoadingID(null);
    }
  }

  useEffect(() => {
    void refresh();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const res = await listUserTokens();
        if (!res.success) return;
        const list = res.data || [];
        const m: Record<number, UserToken> = {};
        for (const tok of list) {
          m[tok.id] = tok;
        }
        if (cancelled) return;
        setTokenByID(m);
      } catch {
        // ignore
      }
    })();
    return () => {
      cancelled = true;
    };
  }, []);

  const rangeSinceText = data ? formatLocalDateTimeMinute(String(data.since)) : '';
  const rangeUntilText = data ? formatLocalDateTimeMinute(String(data.until)) : '';

  const selfEmail = (user?.email || user?.username || '').toString().trim() || '-';
  const selfID = typeof user?.id === 'number' ? user.id : '-';

  const onPrevPage = () => {
    const nextStack = beforeStack.slice(0, -1);
    setBeforeStack(nextStack);
    setExpandedID(null);
    const nextBefore = nextStack.length > 0 ? nextStack[nextStack.length - 1] : undefined;
    void refresh(nextBefore);
  };

  const onNextPage = () => {
    if (!nextBeforeID) return;
    setBeforeStack((s) => [...s, nextBeforeID]);
    setExpandedID(null);
    void refresh(nextBeforeID);
  };

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <div>
          <div className="d-flex justify-content-between align-items-center mb-3">
            <div>
              <h3 className="mb-1 fw-bold">用量统计</h3>
              <div className="text-muted small">按日期范围汇总用量，并支持事件明细查看。</div>
            </div>
          </div>

          {err ? (
            <div className="alert alert-danger mb-3">
              <span className="me-2 material-symbols-rounded">warning</span>
              {err}
            </div>
          ) : null}

          <div className="card border-0 shadow-sm mb-0">
            <div className="card-body py-3 px-4">
              <div className="d-flex flex-wrap align-items-end gap-3">
                <div className="d-flex flex-wrap align-items-center gap-2">
                  <div className="text-muted smaller fw-medium text-nowrap">时间区间</div>
                  <DateRangePicker
                    start={start}
                    end={end}
                    onChange={(r) => {
                      const isAll = !r.start.trim() && !r.end.trim();
                      setAllTime(isAll);
                      setStart(r.start);
                      setEnd(r.end);
                      setBeforeStack([]);
                      setExpandedID(null);
                    }}
                    loading={loading}
                  />
                </div>

                <div className="d-flex flex-wrap align-items-center gap-2">
                  <div className="text-muted smaller fw-medium text-nowrap">显示条数</div>
                  <SelectPicker
                    value={limit}
                    options={[
                      { label: '20', value: 20 },
                      { label: '50', value: 50 },
                      { label: '100', value: 100 },
                    ]}
                    label="条"
                    onChange={(val) => {
                      setLimit(val);
                      setBeforeStack([]);
                      setExpandedID(null);
                    }}
                  />
                </div>

                <div className="d-flex align-items-center gap-2">
                  <UsageAdvancedFiltersDropdown
                    ref={advRef}
                    disabled={loading}
                    toggleTestId="usage-adv-toggle"
                    fields={[
                      {
                        inputId: 'usageFilterKeyValue',
                        label: 'Key',
                        title: 'Key 名称',
                        placeholder: '输入 Key 名称',
                        value: filterKey,
                        onChange: (v) => {
                          setFilterKey(v);
                          setBeforeStack([]);
                          setExpandedID(null);
                        },
                      },
                      {
                        inputId: 'usageFilterModelValue',
                        label: '模型',
                        title: '模型',
                        placeholder: '输入模型名',
                        value: filterModel,
                        onChange: (v) => {
                          setFilterModel(v);
                          setBeforeStack([]);
                          setExpandedID(null);
                        },
                      },
                    ]}
                  />
                </div>

                <div className="ms-auto d-flex gap-2">
                  <button
                    className="btn btn-primary btn-sm"
                    type="button"
                    disabled={loading}
                    onClick={() => {
                      setBeforeStack([]);
                      setExpandedID(null);
                      void refresh(undefined);
                    }}
                  >
                    <span className="material-symbols-rounded me-1">refresh</span>
                    更新
                  </button>
                  <button
                    className="btn btn-light border btn-sm"
                    type="button"
                    disabled={loading}
                    onClick={() => {
                      const today = todayDateInputLocal();
                      setAllTime(false);
                      setStart(today);
                      setEnd(today);
                      advRef.current?.close();
                      setFilterKey('');
                      setFilterModel('');
                      setBeforeStack([]);
                      setExpandedID(null);
                      void refresh(undefined, {
                        start: today,
                        end: today,
                        allTime: false,
                        filterKey: '',
                        filterModel: '',
                      });
                    }}
                  >
                    重置
                  </button>
                </div>
              </div>
            </div>
          </div>
        </div>

        {loading ? (
          <div className="text-muted">加载中…</div>
        ) : data ? (
          <div className="row g-4">
            <div className="col-12">
              <UsageSummaryCard data={data} rangeSinceText={rangeSinceText} rangeUntilText={rangeUntilText} />
            </div>

            <div className="col-12">
              <UsageEventsCard
                events={events}
                tokenByID={tokenByID}
                expandedID={expandedID}
                setExpandedID={setExpandedID}
                loadDetail={loadDetail}
                detailLoadingID={detailLoadingID}
                detailByEventID={detailByEventID}
                canPrev={canPrev}
                canNext={canNext}
                loading={loading}
                onPrevPage={onPrevPage}
                onNextPage={onNextPage}
                selfEmail={selfEmail}
                selfID={selfID}
              />
            </div>
          </div>
        ) : null}
      </SegmentedFrame>
    </div>
  );
}
