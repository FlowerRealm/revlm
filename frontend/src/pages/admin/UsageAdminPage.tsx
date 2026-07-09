import { useEffect, useRef, useState } from 'react';

import { useAuth } from '../../auth/AuthContext';
import {
  getAdminUsageEventDetail,
  getAdminUsagePage,
  type AdminUsagePage,
  type UsageEventDetail,
} from '../../api/admin/usage';
import { SegmentedFrame } from '../../components/SegmentedFrame';
import type { UsageAdvancedFiltersDropdownHandle } from '../../components/UsageAdvancedFiltersDropdown';
import { UsageAdminEventsCard } from './usage/UsageAdminEventsCard';
import { UsageAdminFilterBar } from './usage/UsageAdminFilterBar';
import { UsageAdminSummaryCard } from './usage/UsageAdminSummaryCard';
import { UsageAdminTopUsersCard } from './usage/UsageAdminTopUsersCard';
import { buildAdminUsagePageParams, type UsageAdminCursorOverride } from './usage/usageAdminUtils';

export function UsageAdminPage() {
  useAuth();

  const [data, setData] = useState<AdminUsagePage | null>(null);
  const [loading, setLoading] = useState(true);
  const [err, setErr] = useState('');

  const [start, setStart] = useState('');
  const [end, setEnd] = useState('');
  const [allTime, setAllTime] = useState(false);
  const [limit, setLimit] = useState(50);
  const [beforeID, setBeforeID] = useState<number | undefined>(undefined);
  const [afterID, setAfterID] = useState<number | undefined>(undefined);
  const [filterUser, setFilterUser] = useState('');
  const [filterUserID, setFilterUserID] = useState<number | undefined>(undefined);
  const [filterChannel, setFilterChannel] = useState('');
  const [filterModel, setFilterModel] = useState('');
  const [filterChannelID, setFilterChannelID] = useState<number | undefined>(undefined);
  const [filterModelExact, setFilterModelExact] = useState<string | undefined>(undefined);
  const advRef = useRef<UsageAdvancedFiltersDropdownHandle | null>(null);

  const [expandedID, setExpandedID] = useState<number | null>(null);
  const [detailByEventID, setDetailByEventID] = useState<Record<number, UsageEventDetail>>({});
  const [detailLoadingID, setDetailLoadingID] = useState<number | null>(null);

  async function refresh(opts?: {
    keepCursor?: boolean;
    override?: Parameters<typeof buildAdminUsagePageParams>[1];
    cursor?: UsageAdminCursorOverride;
  }) {
    setErr('');
    setLoading(true);
    try {
      const { params, allTimeActive, startValue, endValue } = buildAdminUsagePageParams(
        {
          start,
          end,
          allTime,
          limit,
          beforeID,
          afterID,
          filterUser,
          filterUserID,
          filterChannel,
          filterChannelID,
          filterModel,
          filterModelExact,
        },
        opts?.override,
        opts?.keepCursor,
        opts?.cursor
      );

      const res = await getAdminUsagePage(params);
      if (!res.success) throw new Error(res.message || '加载失败');
      const nextData = res.data || null;
      setData((prev) => {
        if (!nextData) return null;
        if (params.summary === false) {
          return {
            ...nextData,
            window: nextData.window ?? prev?.window,
            top_users: nextData.top_users ?? prev?.top_users ?? [],
          };
        }
        return nextData;
      });
      if (nextData && !allTimeActive) {
        if (!startValue) setStart(nextData.start || '');
        if (!endValue) setEnd(nextData.end || '');
      }
    } catch (error) {
      setErr(error instanceof Error ? error.message : '加载失败');
      setData(null);
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => {
    void refresh();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const windowStats = data?.window;
  const topUsers = data?.top_users || [];
  const events = data?.events || [];
  const canPrev = typeof data?.prev_after_id === 'number' && (data?.prev_after_id || 0) > 0;
  const canNext = typeof data?.next_before_id === 'number' && (data?.next_before_id || 0) > 0;

  async function loadDetail(eventID: number) {
    if (detailByEventID[eventID]) return;
    setDetailLoadingID(eventID);
    try {
      const res = await getAdminUsageEventDetail(eventID);
      if (!res.success) throw new Error(res.message || '加载详情失败');
      const detail = res.data;
      if (detail) {
        setDetailByEventID((prev) => ({ ...prev, [eventID]: detail }));
      }
    } catch (error) {
      setErr(error instanceof Error ? error.message : '加载详情失败');
    } finally {
      setDetailLoadingID(null);
    }
  }

  function resetCursor() {
    setBeforeID(undefined);
    setAfterID(undefined);
  }

  function handleDateRangeChange(range: { start: string; end: string }) {
    const isAll = !range.start.trim() && !range.end.trim();
    setAllTime(isAll);
    setStart(range.start);
    setEnd(range.end);
    resetCursor();
  }

  function handleUserChange(value: string) {
    setFilterUser(value);
    setFilterUserID(undefined);
    resetCursor();
  }

  function handleChannelChange(value: string) {
    setFilterChannel(value);
    setFilterChannelID(undefined);
    resetCursor();
  }

  function handleModelChange(value: string) {
    setFilterModel(value);
    setFilterModelExact(undefined);
    resetCursor();
  }

  function handleRefresh() {
    resetCursor();
    void refresh();
  }

  function handleReset() {
    setStart('');
    setEnd('');
    setAllTime(false);
    advRef.current?.close();
    setFilterUser('');
    setFilterUserID(undefined);
    setFilterChannel('');
    setFilterModel('');
    setFilterChannelID(undefined);
    setFilterModelExact(undefined);
    resetCursor();
    void refresh({
      override: {
        start: '',
        end: '',
        allTime: false,
        filterUser: '',
        filterUserID: undefined,
        filterChannel: '',
        filterChannelID: undefined,
        filterModel: '',
        filterModelExact: undefined,
      },
      cursor: {
        beforeID: undefined,
        afterID: undefined,
      },
    });
  }

  function handleToggleEvent(eventID: number) {
    const next = expandedID === eventID ? null : eventID;
    setExpandedID(next);
    if (next) void loadDetail(eventID);
  }

  function handlePrevPage() {
    const nextAfterID = data?.prev_after_id;
    if (!nextAfterID) return;
    setBeforeID(undefined);
    setAfterID(nextAfterID);
    void refresh({
      keepCursor: true,
      cursor: { beforeID: undefined, afterID: nextAfterID },
    });
  }

  function handleNextPage() {
    const nextBeforeID = data?.next_before_id;
    if (!nextBeforeID) return;
    setAfterID(undefined);
    setBeforeID(nextBeforeID);
    void refresh({
      keepCursor: true,
      cursor: { beforeID: nextBeforeID, afterID: undefined },
    });
  }

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <div>
          <div className="d-flex justify-content-between align-items-center mb-4">
            <div>
              <h3 className="mb-1 fw-bold">全站用量统计</h3>
              <div className="text-muted small">系统级数据汇总，涵盖所有用户及上游通道。</div>
            </div>
          </div>

          {err ? (
            <div className="alert alert-danger mb-3">
              <span className="me-2 material-symbols-rounded">warning</span>
              {err}
            </div>
          ) : null}

          <UsageAdminFilterBar
            advRef={advRef}
            start={start}
            end={end}
            allTime={allTime}
            loading={loading}
            limit={limit}
            filterUser={filterUser}
            filterChannel={filterChannel}
            filterModel={filterModel}
            onDateRangeChange={handleDateRangeChange}
            onLimitChange={setLimit}
            onUserChange={handleUserChange}
            onChannelChange={handleChannelChange}
            onModelChange={handleModelChange}
            onRefresh={handleRefresh}
            onReset={handleReset}
          />
        </div>

        {loading ? (
          <div className="text-muted">加载中…</div>
        ) : data && windowStats ? (
          <div className="row g-4">
            <div className="col-12">
              <UsageAdminSummaryCard windowStats={windowStats} />
            </div>

            <div className="col-12">
              <UsageAdminTopUsersCard topUsers={topUsers} />
            </div>

            <div className="col-12">
              <UsageAdminEventsCard
                events={events}
                expandedID={expandedID}
                detailByEventID={detailByEventID}
                detailLoadingID={detailLoadingID}
                canPrev={canPrev}
                canNext={canNext}
                loading={loading}
                onToggleEvent={handleToggleEvent}
                onPrevPage={handlePrevPage}
                onNextPage={handleNextPage}
              />
            </div>
          </div>
        ) : null}
      </SegmentedFrame>
    </div>
  );
}
