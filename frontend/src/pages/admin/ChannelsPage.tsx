import { Fragment, useCallback, useEffect, useMemo, useRef, useState, type MutableRefObject } from 'react';

import { useAuth } from '../../auth/AuthContext';
import { BootstrapModal } from '../../components/BootstrapModal';
import { SegmentedFrame } from '../../components/SegmentedFrame';
import { closeModalById, showModalById } from '../../components/modal';
import { formatSecondsFromMilliseconds } from '../../format/duration';
import { formatIntComma } from '../../format/int';
import {
  deleteChannel,
  getChannelsPage,
  getChannelTimeSeries,
  updateChannel,
  type Channel,
  type ChannelItem,
  type ChannelTimeSeriesPoint,
} from '../../api/channels';
import {
  listAdminChannelGroups,
  upsertAdminChannelGroupPointer,
  type AdminChannelGroup,
} from '../../api/admin/channelGroups';
import { DateRangePicker } from '../../components/DateRangePicker';
import { fillDailyBuckets } from '../../utils/timeSeries';
import { ChannelCommonTab } from './channels/ChannelCommonTab';
import { parseGroupsCSV } from './channels/utils';

function statusBadge(status: boolean): { cls: string; label: string } {
  if (status)
    return {
      cls: 'badge bg-success bg-opacity-10 text-success border border-success-subtle',
      label: '启用',
    };
  return {
    cls: 'badge bg-secondary bg-opacity-10 text-secondary border',
    label: '禁用',
  };
}

type ChannelPatch = Partial<{
  type: string;
  name: string;
  status: boolean;
  base_url: string;
  groups: string;
  priority: number;
  api_key: string;
  price_multiplier: number;
  config_json: Record<string, unknown>;
}>;

type ChartInstance = {
  destroy?: () => void;
};

type ChartConstructor = new (ctx: CanvasRenderingContext2D, config: unknown) => ChartInstance;

type ChannelPointerTarget = {
  id: number;
  name: string;
  groups: string;
};

export function ChannelsPage() {
  useAuth();
  const channelTableCols = 3;

  const [channels, setChannels] = useState<ChannelItem[]>([]);

  const [channelGroups, setChannelGroups] = useState<AdminChannelGroup[]>([]);
  const [loading, setLoading] = useState(true);
  const [expandedChannelID, setExpandedChannelID] = useState<number | null>(null);
  const [pointerTarget, setPointerTarget] = useState<ChannelPointerTarget | null>(null);
  const [pointerGroupID, setPointerGroupID] = useState('');

  const [usageStart, setUsageStart] = useState('');
  const [usageEnd, setUsageEnd] = useState('');
  const [usageAllTime, setUsageAllTime] = useState(false);
  const [usageRangeCustomized, setUsageRangeCustomized] = useState(false);
  const [usageRangeDirty, setUsageRangeDirty] = useState(false);
  const detailTimeLineRef = useRef<HTMLCanvasElement | null>(null);
  const detailTimeLineChartRef = useRef<ChartInstance | null>(null);
  const oauthQueryHandled = useRef(false);
  const [detailSeries, setDetailSeries] = useState<ChannelTimeSeriesPoint[]>([]);
  const [detailSeriesStart, setDetailSeriesStart] = useState('');
  const [detailSeriesEnd, setDetailSeriesEnd] = useState('');
  const [detailSeriesLoading, setDetailSeriesLoading] = useState(false);
  const [detailSeriesErr, setDetailSeriesErr] = useState('');
  const [detailPanelByChannel, setDetailPanelByChannel] = useState<Record<number, 'stats' | 'accounts'>>({});
  const [detailField, setDetailField] = useState<'usd' | 'avg_first_token_latency'>('usd');
  const [detailGranularity, setDetailGranularity] = useState<'hour' | 'day'>('hour');
  const fieldOptions: Array<{
    value: 'usd' | 'avg_first_token_latency';
    label: string;
  }> = [
    { value: 'usd', label: '消耗 (USD)' },
    { value: 'avg_first_token_latency', label: '首字延迟 (s)' },
  ];
  const granularityOptions: Array<{ value: 'hour' | 'day'; label: string }> = [
    { value: 'hour', label: '按小时' },
    { value: 'day', label: '按天' },
  ];

  const [creating, setCreating] = useState(false);

  const [settingsChannelID, setSettingsChannelID] = useState<number | null>(null);
  const [settingsChannelName, setSettingsChannelName] = useState('');
  const [settingsChannel, setSettingsChannel] = useState<Channel | null>(null);
  const [settingsLoading, setSettingsLoading] = useState(false);

  const [editName, setEditName] = useState('');
  const [editType, setEditType] = useState('');
  const [editGroups, setEditGroups] = useState('');
  const [editBaseURL, setEditBaseURL] = useState('');
  const [editKey, setEditKey] = useState('');
  const [editStatus, setEditStatus] = useState(true);
  const [editPriority, setEditPriority] = useState('0');
  const [editPriceMultiplier, setEditPriceMultiplier] = useState('1');
  const [editConfigJSON, setEditConfigJSON] = useState<Record<string, unknown>>({});

  const applyChannelPatch = useCallback(
    (id: number, patch: ChannelPatch) => {
      if (patch.name !== undefined) setSettingsChannelName(patch.name);
      setSettingsChannel((prev) => (prev && prev.id === id ? ({ ...prev, ...patch } as Channel) : prev));
      setChannels((prev) => prev.map((c) => (c.id === id ? ({ ...c, ...patch } as ChannelItem) : c)));
    },
    [setChannels, setSettingsChannel, setSettingsChannelName]
  );

  const enabledCount = useMemo(() => channels.filter((c) => c.status).length, [channels]);
  const disabledCount = useMemo(() => channels.length - enabledCount, [channels.length, enabledCount]);
  const firstDisabledIndex = useMemo(() => channels.findIndex((c) => !c.status), [channels]);
  function normalizeChannelSections(list: ChannelItem[]): ChannelItem[] {
    const enabled = list.filter((ch) => ch.status);
    const disabled = list.filter((ch) => !ch.status);
    return [...enabled, ...disabled];
  }

  function toggleChannelPanel(channelID: number) {
    if (expandedChannelID === channelID) {
      setExpandedChannelID(null);
      return;
    }
    setExpandedChannelID(channelID);
  }

  const refresh = useCallback(async (params?: { start?: string; end?: string; all_time?: boolean }) => {
    setLoading(true);
    try {
      const startValue = (params?.start ?? '').trim();
      const endValue = (params?.end ?? '').trim();
      const allTimeActive = !!params?.all_time;
      const pageParams = allTimeActive
        ? { all_time: true }
        : { start: startValue || undefined, end: endValue || undefined };

      const pageRes = await getChannelsPage(pageParams);
      if (!pageRes.success) throw new Error(pageRes.message || '加载渠道失败');
      const pageChannels = pageRes.data?.channels || [];
      const normalizedChannels = normalizeChannelSections(pageChannels);
      if (!allTimeActive) {
        setUsageStart(pageRes.data?.start || '');
        setUsageEnd(pageRes.data?.end || '');
      }
      setChannels(normalizedChannels);
      return normalizedChannels;
    } catch {
      // keep the current table when refresh fails
      return [] as ChannelItem[];
    } finally {
      setLoading(false);
    }
  }, []);

  const refreshWithCurrentRange = useCallback(async () => {
    const startValue = usageStart.trim();
    const endValue = usageEnd.trim();
    const allTimeValue = usageAllTime;
    return refresh({ start: startValue, end: endValue, all_time: allTimeValue });
  }, [refresh, usageAllTime, usageEnd, usageStart]);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  useEffect(() => {
    if (!usageRangeDirty) return;
    const t = window.setTimeout(() => {
      setUsageRangeDirty(false);
      void refreshWithCurrentRange();
    }, 400);
    return () => window.clearTimeout(t);
  }, [usageRangeDirty, refreshWithCurrentRange]);

  useEffect(() => {
    if (!expandedChannelID) {
      setDetailSeries([]);
      setDetailSeriesStart('');
      setDetailSeriesEnd('');
      setDetailSeriesErr('');
      setDetailSeriesLoading(false);
      return;
    }
    let active = true;
    void (async () => {
      setDetailSeriesErr('');
      setDetailSeriesLoading(true);
      try {
        const allTimeActive = usageAllTime && !usageStart.trim() && !usageEnd.trim();
        const implicitDayRange = detailGranularity === 'day' && !usageRangeCustomized && !allTimeActive;
        const res = await getChannelTimeSeries(expandedChannelID, {
          start: allTimeActive || implicitDayRange ? undefined : usageStart.trim() || undefined,
          end: allTimeActive || implicitDayRange ? undefined : usageEnd.trim() || undefined,
          all_time: allTimeActive ? true : undefined,
          granularity: detailGranularity,
        });
        if (!res.success) throw new Error(res.message || '加载时间序列失败');
        if (!active) return;
        const startValue = res.data?.start || '';
        const endValue = res.data?.end || '';
        const points = res.data?.points || [];
        setDetailSeriesStart(startValue);
        setDetailSeriesEnd(endValue);
        setDetailSeries(
          detailGranularity === 'day'
            ? fillDailyBuckets(points, startValue, endValue, (bucket) => ({
                bucket,
                usd: '0',
                avg_first_token_latency: '0',
              }))
            : points
        );
      } catch (e) {
        if (!active) return;
        setDetailSeries([]);
        setDetailSeriesStart('');
        setDetailSeriesEnd('');
        setDetailSeriesErr(e instanceof Error ? e.message : '加载时间序列失败');
      } finally {
        if (active) setDetailSeriesLoading(false);
      }
    })();
    return () => {
      active = false;
    };
  }, [expandedChannelID, usageAllTime, usageRangeCustomized, usageStart, usageEnd, detailGranularity]);

  useEffect(() => {
    void (async () => {
      try {
        const res = await listAdminChannelGroups();
        if (res.success) setChannelGroups(res.data || []);
      } catch {
        // ignore
      }
    })();
  }, []);

  const channelGroupByName = useMemo(() => {
    const m = new Map<string, AdminChannelGroup>();
    for (const g of channelGroups) {
      const name = (g.name || '').trim();
      if (!name) continue;
      if (m.has(name)) continue;
      m.set(name, g);
    }
    return m;
  }, [channelGroups]);

  const pointerGroupOptions = useMemo(() => {
    if (!pointerTarget) return [];
    const names = parseGroupsCSV(pointerTarget.groups || '');
    const out: AdminChannelGroup[] = [];
    for (const name of names) {
      const g = channelGroupByName.get(name);
      if (!g || !g.status) continue;
      out.push(g);
    }
    return out;
  }, [pointerTarget, channelGroupByName]);

  const openChannelSettingsModal = useCallback((ch: Channel) => {
    setSettingsChannelID(ch.id);
    setSettingsChannelName(ch.name || `#${ch.id}`);
    setSettingsChannel(ch);
    setEditType(ch.type || '');
    setEditName(ch.name || '');
    setEditGroups(ch.groups || '');
    setEditBaseURL(ch.base_url || '');
    setEditKey(ch.api_key || '');
    setEditStatus(!!ch.status);
    setEditPriority(String(ch.priority || 0));
    setEditPriceMultiplier(String(ch.price_multiplier ?? 1));
    setEditConfigJSON(ch.config_json || {});

    if (typeof window === 'undefined') return;
    const modalRoot = document.getElementById('editChannelModal');
    const modalCtor = (
      window as Window & {
        bootstrap?: {
          Modal?: {
            getOrCreateInstance: (el: Element) => { show: () => void };
          };
        };
      }
    ).bootstrap?.Modal;
    if (!modalRoot || !modalCtor?.getOrCreateInstance) return;
    modalCtor.getOrCreateInstance(modalRoot).show();
  }, []);

  const handleStartCreate = useCallback(() => {
    setCreating(true);
    setEditType('');
    setEditName('');
    setEditGroups('');
    setEditBaseURL('');
    setEditKey('');
    setEditStatus(true);
    setEditPriority('0');
    setEditPriceMultiplier('1');
    setEditConfigJSON({});
    showModalById('createChannelModal');
  }, []);

  useEffect(() => {
    if (oauthQueryHandled.current || loading) return;
    if (typeof window === 'undefined') return;
    oauthQueryHandled.current = true;

    const params = new URLSearchParams(window.location.search);
    const openChannelSettings = Number.parseInt(params.get('open_channel_settings') || '', 10);
    const oauthState = (params.get('oauth') || '').trim();
    const oauthErr = (params.get('err') || '').trim();

    if (openChannelSettings > 0) {
      const target = channels.find((ch) => ch.id === openChannelSettings);
      if (target) openChannelSettingsModal(target);
    }

    if (openChannelSettings > 0 || oauthState !== '' || oauthErr !== '') {
      params.delete('open_channel_settings');
      params.delete('oauth');
      params.delete('err');
      const nextQuery = params.toString();
      const nextURL = `${window.location.pathname}${nextQuery ? `?${nextQuery}` : ''}${window.location.hash || ''}`;
      window.history.replaceState({}, '', nextURL);
    }
  }, [channels, loading, openChannelSettingsModal]);

  const loadChannelSettings = useCallback(
    async (channelID: number) => {
      setSettingsLoading(true);
      try {
        const ch = channels.find((item) => item.id === channelID);
        if (!ch) throw new Error('渠道不存在');
        setSettingsChannel(ch);

        setEditType(ch.type || '');
        setEditName(ch.name || '');
        setEditGroups(ch.groups || '');
        setEditBaseURL(ch.base_url || '');
        setEditKey(ch.api_key || '');
        setEditStatus(!!ch.status);
        setEditPriority(String(ch.priority || 0));
        setEditPriceMultiplier(String(ch.price_multiplier ?? 1));
        setEditConfigJSON(ch.config_json || {});
      } catch {
        setSettingsChannel(null);
      } finally {
        setSettingsLoading(false);
      }
    },
    [channels]
  );

  useEffect(() => {
    if (!settingsChannelID) return;
    void loadChannelSettings(settingsChannelID);
  }, [settingsChannelID, loadChannelSettings]);

  const handleSettingsModalHidden = useCallback(() => {
    setSettingsChannelID(null);
    setSettingsChannelName('');
    setSettingsChannel(null);
    setSettingsLoading(false);

    setEditKey('');
    setEditConfigJSON({});
  }, []);

  useEffect(() => {
    const ChartCtor = (window as unknown as { Chart?: ChartConstructor }).Chart;

    const destroy = (ref: MutableRefObject<ChartInstance | null>) => {
      try {
        ref.current?.destroy?.();
      } catch {
        // ignore
      }
      ref.current = null;
    };

    destroy(detailTimeLineChartRef);

    if (!ChartCtor || !expandedChannelID) return;
    const channel = channels.find((c) => c.id === expandedChannelID);
    if (!channel) return;
    const canvas = detailTimeLineRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const css = getComputedStyle(canvas);
    const rgb = (varName: string, fallback: string) => (css.getPropertyValue(varName).trim() || fallback).trim();
    const color = (rgbValue: string, alpha: number) => `rgba(${rgbValue}, ${alpha})`;
    const palette = {
      success: rgb('--bs-success-rgb', '47, 107, 75'),
      warning: rgb('--bs-warning-rgb', '122, 98, 50'),
      danger: rgb('--bs-danger-rgb', '122, 52, 52'),
      primary: rgb('--bs-primary-rgb', '60, 138, 97'),
      secondary: rgb('--bs-secondary-rgb', '99, 116, 107'),
    };

    const fieldMeta: Record<
      string,
      {
        label: string;
        color: string;
        read: (p: ChannelTimeSeriesPoint) => number;
      }
    > = {
      usd: {
        label: '消耗 (USD)',
        color: color(palette.primary, 0.95),
        // Decimal strings on the wire: money never travels as a double.
        read: (p) => Number(p.usd) || 0,
      },
      avg_first_token_latency: {
        label: '首字延迟 (s)',
        color: color(palette.danger, 0.95),
        read: (p) => (Number(p.avg_first_token_latency) || 0) / 1000,
      },
    };
    const meta = fieldMeta[detailField];
    const datasets = [
      {
        label: meta.label,
        data: detailSeries.map((p) => meta.read(p)),
        borderColor: meta.color,
        backgroundColor: meta.color.replace('0.95', '0.18'),
        pointRadius: 2,
        tension: 0.2,
      },
    ];

    detailTimeLineChartRef.current = new ChartCtor(ctx, {
      type: 'line',
      data: {
        labels: detailSeries.map((p) => p.bucket),
        datasets,
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: 'index', intersect: false },
        plugins: {
          legend: { position: 'bottom' },
          title: {
            display: true,
            text: `${channel.name || `渠道 #${channel.id}`} · 时间序列`,
          },
        },
        scales: {
          x: {
            grid: { display: false },
            ticks: {
              autoSkip: true,
              maxTicksLimit: detailGranularity === 'hour' ? 10 : 14,
              maxRotation: 0,
              minRotation: 0,
            },
          },
          y: {
            beginAtZero: true,
            grid: { color: color(palette.secondary, 0.18) },
            ...(detailField === 'usd' ? {} : {}),
          },
        },
      },
    });

    return () => {
      destroy(detailTimeLineChartRef);
    };
  }, [channels, expandedChannelID, detailSeries, detailField, detailGranularity]);

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <div className="d-flex justify-content-between align-items-start flex-wrap gap-3">
          <div>
            <h2 className="h4 fw-bold mb-1">上游渠道管理</h2>
            <p className="text-muted small mb-0">
              管理模型转发渠道。当前 {formatIntComma(enabledCount)} 启用 / {formatIntComma(disabledCount)} 禁用 /{' '}
              {formatIntComma(channels.length)} 总计。
            </p>
          </div>
          <button type="button" className="btn btn-primary" disabled={creating} onClick={handleStartCreate}>
            <i className="ri-add-line me-1"></i>
            新建渠道
          </button>
        </div>

        <div
          className="d-flex flex-wrap align-items-center gap-2 mb-0 bg-white p-2 rounded-3 border-light shadow-sm mt-3"
          style={{ border: '1px solid #f1f3f5' }}
        >
          <div className="d-flex align-items-center px-2">
            <span className="small text-muted me-2" style={{ whiteSpace: 'nowrap', fontSize: '12px' }}>
              统计区间
            </span>
            <DateRangePicker
              start={usageStart}
              end={usageEnd}
              onChange={(r) => {
                const isAll = !r.start.trim() && !r.end.trim();
                setUsageRangeCustomized(true);
                setUsageAllTime(isAll);
                if (isAll) setDetailGranularity('day');
                setUsageStart(r.start);
                setUsageEnd(r.end);
                setUsageRangeDirty(true);
              }}
              loading={loading}
            />
          </div>

          <div className="ms-auto d-flex gap-2 pe-1">
            <button
              className="btn btn-sm"
              style={{
                backgroundColor: '#326c52',
                color: '#ffffff',
                fontWeight: 500,
                height: '28px',
                fontSize: '12px',
                display: 'flex',
                alignItems: 'center',
                borderRadius: '4px',
                padding: '0 12px',
                transition: 'all 0.2s',
                border: 'none',
              }}
              type="button"
              disabled={loading}
              onClick={() => {
                void refreshWithCurrentRange();
              }}
            >
              <span className="material-symbols-rounded me-1" style={{ fontSize: '16px' }}>
                refresh
              </span>
              刷新数据
            </button>
            <button
              className="btn btn-sm"
              style={{
                height: '28px',
                fontSize: '12px',
                border: '1px solid #e9ecef',
                borderRadius: '4px',
                backgroundColor: '#ffffff',
                color: '#6c757d',
                padding: '0 12px',
                display: 'flex',
                alignItems: 'center',
                transition: 'all 0.2s',
              }}
              type="button"
              disabled={loading}
              onClick={() => {
                setUsageAllTime(false);
                setUsageRangeCustomized(false);
                setUsageStart('');
                setUsageEnd('');
                setUsageRangeDirty(true);
              }}
            >
              重置
            </button>
          </div>
        </div>

        <div>
          <div className="card border-0 shadow-sm overflow-hidden mb-0">
            <div className="bg-primary bg-opacity-10 py-3 px-4 d-flex justify-content-between align-items-center">
              <div>
                <span className="text-primary fw-bold text-uppercase small">渠道列表</span>
              </div>
            </div>
            <div className="table-responsive">
              <table className="table table-hover align-middle mb-0">
                <thead className="table-light">
                  <tr>
                    <th className="ps-4">渠道详情</th>
                    <th>状态</th>
                    <th className="text-end pe-4">操作</th>
                  </tr>
                </thead>
                <tbody>
                  {loading ? (
                    <tr>
                      <td colSpan={channelTableCols} className="text-center py-5 text-muted">
                        加载中…
                      </td>
                    </tr>
                  ) : channels.length === 0 ? (
                    <tr>
                      <td colSpan={channelTableCols} className="text-center py-5 text-muted">
                        <span className="fs-1 d-block mb-3 material-symbols-rounded">inbox</span>
                        暂无渠道。
                      </td>
                    </tr>
                  ) : (
                    <>
                      {channels.map((ch, idx) => {
                        const st = statusBadge(ch.status);
                        const channelDisabled = !ch.status;
                        const runtime = ch.runtime;
                        const usage = ch.usage;
                        const panelOpen = expandedChannelID === ch.id;
                        const detailPanel = detailPanelByChannel[ch.id] || 'stats';
                        const rowBaseClassName = [
                          'rlm-channel-row-main',
                          channelDisabled ? 'table-secondary opacity-75' : '',
                        ]
                          .filter((v) => v)
                          .join(' ');
                        const groupNames = parseGroupsCSV(ch.groups || '');
                        const pointerGroups = groupNames
                          .map((name) => channelGroupByName.get(name))
                          .filter((g): g is AdminChannelGroup => !!g && g.status);
                        const canSetPointer = !channelDisabled && pointerGroups.length > 0;
                        const setPointerTitle = channelDisabled
                          ? '禁用渠道不可设为指针'
                          : pointerGroups.length === 0
                            ? '该渠道未加入任何启用的渠道组'
                            : '设为指针';

                        const renderMainRowCells = () => (
                          <>
                            <td className="ps-4" style={{ minWidth: 0 }}>
                              <div className="d-flex flex-column">
                                <div className="d-flex flex-wrap align-items-center gap-2">
                                  <span className="fw-bold text-dark">{ch.name || `渠道 #${ch.id}`}</span>
                                  <span className="text-muted small">({ch.type})</span>
                                  {ch.in_use ? (
                                    <span className="badge bg-info bg-opacity-10 text-info border border-info-subtle">
                                      使用中
                                    </span>
                                  ) : null}
                                </div>
                                <div className="d-flex flex-wrap align-items-center gap-2 small text-muted mt-1">
                                  {ch.base_url ? (
                                    <span
                                      className="font-monospace d-inline-block user-select-all"
                                      style={{
                                        maxWidth: 360,
                                        whiteSpace: 'nowrap',
                                        overflow: 'hidden',
                                        textOverflow: 'ellipsis',
                                      }}
                                      title={ch.base_url}
                                    >
                                      {ch.base_url}
                                    </span>
                                  ) : null}
                                  <div className="d-flex align-items-center">
                                    {ch.base_url ? <span className="text-secondary">·</span> : null}
                                    <span className={`${ch.base_url ? 'ms-2 ' : ''}me-1`}>渠道组:</span>
                                    <span className="text-secondary font-monospace user-select-all">
                                      {(ch.groups || '').trim() || '-'}
                                    </span>
                                  </div>
                                </div>
                              </div>
                            </td>
                            <td>
                              <span className={st.cls}>{st.label}</span>
                              {runtime?.available && runtime.banned_active ? (
                                <div className="mt-1">
                                  <span
                                    className="badge bg-warning-subtle text-warning-emphasis border px-2"
                                    title={runtime.banned_until ? `封禁至 ${runtime.banned_until}` : undefined}
                                  >
                                    <i className="ri-forbid-2-line me-1"></i>
                                    封禁中 · 剩余 {runtime.banned_remaining || '-'}
                                  </span>
                                </div>
                              ) : null}
                              {runtime?.available &&
                              typeof runtime.fail_score === 'number' &&
                              runtime.fail_score > 0 ? (
                                <div className="mt-1">
                                  <span
                                    className="badge bg-light text-secondary border"
                                    title="失败计分（运行态 fail score，越高越容易触发封禁/探测）"
                                  >
                                    失败计分：{runtime.fail_score}
                                  </span>
                                </div>
                              ) : null}
                            </td>
                            <td className="text-end pe-4 text-nowrap">
                              <div className="d-flex gap-1 justify-content-end">
                                <button
                                  className={`btn btn-sm ${ch.status ? 'btn-light border text-warning' : 'btn-light border text-success'}`}
                                  type="button"
                                  title={ch.status ? '禁用渠道' : '启用渠道'}
                                  disabled={loading}
                                  onClick={async () => {
                                    const targetStatus = !ch.status;
                                    try {
                                      const res = await updateChannel({
                                        id: ch.id,
                                        status: targetStatus,
                                      });
                                      if (!res.success) throw new Error(res.message || '更新状态失败');
                                      if (settingsChannelID === ch.id) {
                                        setEditStatus(targetStatus);
                                      }
                                      await refreshWithCurrentRange();
                                    } catch {
                                      // Best-effort refresh; keep the admin action flow silent.
                                    }
                                  }}
                                >
                                  <i
                                    className={`me-1 ${ch.status ? 'ri-pause-circle-line' : 'ri-play-circle-line'}`}
                                  ></i>
                                  {ch.status ? '禁用' : '启用'}
                                </button>

                                <button
                                  className="btn btn-sm btn-light border text-warning"
                                  type="button"
                                  title={setPointerTitle}
                                  disabled={loading || !canSetPointer}
                                  data-bs-toggle="modal"
                                  data-bs-target="#setChannelPointerModal"
                                  onClick={() => {
                                    if (!canSetPointer) return;
                                    setPointerTarget({
                                      id: ch.id,
                                      name: ch.name || `渠道 #${ch.id}`,
                                      groups: ch.groups || '',
                                    });
                                    setPointerGroupID(String(pointerGroups[0].id));
                                  }}
                                >
                                  <i className="ri-pushpin-2-line me-1"></i>指针
                                </button>

                                <button
                                  className="btn btn-sm btn-primary"
                                  type="button"
                                  title="设置"
                                  disabled={loading}
                                  onClick={() => openChannelSettingsModal(ch)}
                                >
                                  <i className="ri-settings-3-line me-1"></i>
                                  设置
                                </button>

                                <button
                                  className="btn btn-sm btn-light border text-danger"
                                  type="button"
                                  title="删除"
                                  disabled={loading}
                                  onClick={async () => {
                                    if (!window.confirm(`确认删除渠道 ${ch.name || ch.id} ? 此操作不可恢复。`)) return;
                                    try {
                                      const res = await deleteChannel(ch.id);
                                      if (!res.success) throw new Error(res.message || '删除失败');
                                      await refreshWithCurrentRange();
                                    } catch {
                                      // Best-effort refresh; keep the admin action flow silent.
                                    }
                                  }}
                                >
                                  <i className="ri-delete-bin-line me-1"></i>
                                  删除
                                </button>
                              </div>
                            </td>
                          </>
                        );

                        return (
                          <Fragment key={ch.id}>
                            {idx === firstDisabledIndex ? (
                              <tr className="table-light">
                                <td colSpan={channelTableCols} className="px-4 py-2">
                                  <span className="text-muted small">
                                    <i className="ri-forbid-2-line me-1"></i>
                                    已禁用渠道（{disabledCount}
                                    ）已固定在底部分区
                                  </span>
                                </td>
                              </tr>
                            ) : null}
                            <tr
                              className={rowBaseClassName || undefined}
                              data-rlm-channel-row="main"
                              data-rlm-channel-id={ch.id}
                              data-rlm-channel-disabled={channelDisabled ? '1' : '0'}
                              onClick={(e) => {
                                const target = e.target as HTMLElement;
                                if (target.closest('button, a, input, textarea, select, label')) return;
                                toggleChannelPanel(ch.id);
                              }}
                            >
                              {renderMainRowCells()}
                            </tr>
                            {panelOpen ? (
                              <tr
                                className={`${channelDisabled ? 'table-secondary opacity-75' : 'bg-light-subtle'} rlm-channel-detail-row`}
                              >
                                <td colSpan={channelTableCols} className="px-4 py-3">
                                  <div className="d-flex flex-wrap align-items-center gap-2 mb-3">
                                    <button
                                      type="button"
                                      className={`btn btn-sm ${detailPanel === 'stats' ? 'btn-primary' : 'btn-light border'}`}
                                      onClick={() =>
                                        setDetailPanelByChannel((prev) => ({
                                          ...prev,
                                          [ch.id]: 'stats',
                                        }))
                                      }
                                    >
                                      详细统计
                                    </button>
                                  </div>

                                  <>
                                    <div className="d-flex flex-wrap align-items-center gap-3 small text-muted">
                                      <div className="d-flex align-items-center">
                                        <span className="me-1">消耗:</span>
                                        <span className="font-monospace fw-bold text-dark">{usage?.usd ?? '0'}</span>
                                      </div>
                                      <div className="d-flex align-items-center">
                                        <span className="me-1">首字:</span>
                                        <span className="fw-medium text-dark">
                                          {formatSecondsFromMilliseconds(usage?.avg_first_token_latency)}
                                        </span>
                                      </div>
                                    </div>
                                    <div className="border rounded-3 p-3 bg-white mt-3">
                                      <div className="d-flex flex-wrap align-items-center gap-3 mb-2">
                                        <div className="d-flex align-items-center gap-2 flex-grow-1">
                                          <div className="d-flex flex-wrap gap-1">
                                            {fieldOptions.map((option) => (
                                              <button
                                                key={option.value}
                                                type="button"
                                                className={`btn btn-sm ${detailField === option.value ? 'btn-primary' : 'btn-outline-secondary'}`}
                                                onClick={() => setDetailField(option.value)}
                                              >
                                                {option.label}
                                              </button>
                                            ))}
                                          </div>
                                        </div>
                                        <div className="d-flex align-items-center gap-2 ms-auto">
                                          <div className="d-flex gap-1">
                                            {granularityOptions.map((option) => (
                                              <button
                                                key={option.value}
                                                type="button"
                                                className={`btn btn-sm ${detailGranularity === option.value ? 'btn-primary' : 'btn-outline-secondary'}`}
                                                onClick={() => setDetailGranularity(option.value)}
                                              >
                                                {option.label}
                                              </button>
                                            ))}
                                          </div>
                                        </div>
                                      </div>
                                      <div className="small text-muted mb-2">
                                        时间区间：{detailSeriesStart || '-'} ~ {detailSeriesEnd || '-'}
                                      </div>
                                      {detailSeriesErr ? (
                                        <div className="alert alert-danger py-2 mb-2">{detailSeriesErr}</div>
                                      ) : null}
                                      {detailSeriesLoading ? (
                                        <div className="text-muted small py-4">时间序列加载中…</div>
                                      ) : (
                                        <>
                                          <div style={{ height: 280 }}>
                                            <canvas ref={panelOpen ? detailTimeLineRef : undefined}></canvas>
                                          </div>
                                        </>
                                      )}
                                    </div>
                                  </>
                                </td>
                              </tr>
                            ) : null}
                          </Fragment>
                        );
                      })}
                    </>
                  )}
                </tbody>
              </table>
            </div>
          </div>
        </div>
      </SegmentedFrame>

      <BootstrapModal
        id="setChannelPointerModal"
        title={pointerTarget ? `设为指针：${pointerTarget.name || `#${pointerTarget.id}`}` : '设为指针'}
        dialogClassName="modal-dialog-centered"
        onHidden={() => {
          setPointerTarget(null);
          setPointerGroupID('');
        }}
      >
        {!pointerTarget ? (
          <div className="text-muted">未选择渠道。</div>
        ) : pointerGroupOptions.length === 0 ? (
          <div className="text-muted">该渠道未加入任何启用的渠道组，无法设为指针。</div>
        ) : (
          <form
            className="row g-3"
            onSubmit={async (e) => {
              e.preventDefault();
              if (!pointerTarget) return;
              const groupID = Number.parseInt(pointerGroupID, 10) || 0;
              if (groupID <= 0) {
                return;
              }
              const g = pointerGroupOptions.find((x) => x.id === groupID) || null;
              if (
                !window.confirm(
                  `确认将渠道 ${pointerTarget.name || pointerTarget.id} 设为渠道组 ${g?.name || groupID} 的指针？`
                )
              )
                return;
              try {
                const res = await upsertAdminChannelGroupPointer(groupID, {
                  channel_id: pointerTarget.id,
                  pinned: true,
                });
                if (!res.success) throw new Error(res.message || '设置失败');
                closeModalById('setChannelPointerModal');
              } catch {
                // Best-effort refresh; keep the admin action flow silent.
              }
            }}
          >
            <div className="col-12">
              <label className="form-label">选择渠道组</label>
              <select
                className="form-select"
                value={pointerGroupID}
                onChange={(e) => setPointerGroupID(e.target.value)}
              >
                {pointerGroupOptions.map((g) => (
                  <option key={g.id} value={String(g.id)}>
                    {g.name} #{g.id}
                  </option>
                ))}
              </select>
              <div className="form-text small text-muted">指针会固定到该渠道，直到被重新设置。</div>
            </div>

            <div className="modal-footer border-top-0 px-0 pb-0">
              <button type="button" className="btn btn-light" data-bs-dismiss="modal">
                取消
              </button>
              <button type="submit" className="btn btn-primary px-4">
                确认设置
              </button>
            </div>
          </form>
        )}
      </BootstrapModal>

      <BootstrapModal
        id="createChannelModal"
        title="新建渠道"
        dialogClassName="modal-dialog-centered modal-lg modal-dialog-scrollable"
        bodyClassName="bg-light"
        footer={
          <button type="button" className="btn btn-light" data-bs-dismiss="modal">
            取消
          </button>
        }
        onHidden={() => {
          setCreating(false);
          setEditKey('');
          setEditConfigJSON({});
        }}
      >
        {!creating ? (
          <div className="text-muted">新建渠道已取消。</div>
        ) : (
          <ChannelCommonTab
            mode="create"
            enabled
            channelGroups={channelGroups}
            editType={editType}
            setEditType={setEditType}
            editName={editName}
            setEditName={setEditName}
            editStatus={editStatus}
            setEditStatus={setEditStatus}
            editBaseURL={editBaseURL}
            setEditBaseURL={setEditBaseURL}
            editKey={editKey}
            setEditKey={setEditKey}
            editGroups={editGroups}
            setEditGroups={setEditGroups}
            editPriority={editPriority}
            setEditPriority={setEditPriority}
            editPriceMultiplier={editPriceMultiplier}
            setEditPriceMultiplier={setEditPriceMultiplier}
            editConfigJSON={editConfigJSON}
            setEditConfigJSON={setEditConfigJSON}
            onCreated={async () => {
              await refreshWithCurrentRange();
              closeModalById('createChannelModal');
            }}
          />
        )}
      </BootstrapModal>

      <BootstrapModal
        id="editChannelModal"
        title={settingsChannelID ? `渠道设置：${settingsChannelName || `#${settingsChannelID}`}` : '渠道设置'}
        dialogClassName="modal-dialog-centered modal-lg modal-dialog-scrollable"
        bodyClassName="bg-light"
        footer={
          <button type="button" className="btn btn-light" data-bs-dismiss="modal">
            关闭
          </button>
        }
        onHidden={() => {
          void handleSettingsModalHidden();
        }}
      >
        {!settingsChannelID ? (
          <div className="text-muted">未选择渠道。</div>
        ) : settingsLoading ? (
          <div className="text-muted">加载中…</div>
        ) : !settingsChannel ? (
          <div className="text-muted">加载失败。</div>
        ) : (
          <>
            <div className="d-flex flex-wrap align-items-center gap-2 mb-3">
              <span className="fw-semibold text-dark">{settingsChannel.name || `渠道 #${settingsChannel.id}`}</span>
              <span className="text-muted small">#{settingsChannel.id}</span>
              <span className="text-muted small">({settingsChannel.type})</span>
            </div>

            <ChannelCommonTab
              enabled={!!settingsChannelID && !!settingsChannel && !settingsLoading}
              channelID={settingsChannelID}
              channelGroups={channelGroups}
              editType={editType}
              setEditType={setEditType}
              editName={editName}
              setEditName={setEditName}
              editStatus={editStatus}
              setEditStatus={setEditStatus}
              editBaseURL={editBaseURL}
              setEditBaseURL={setEditBaseURL}
              editKey={editKey}
              setEditKey={setEditKey}
              editGroups={editGroups}
              setEditGroups={setEditGroups}
              editPriority={editPriority}
              setEditPriority={setEditPriority}
              editPriceMultiplier={editPriceMultiplier}
              setEditPriceMultiplier={setEditPriceMultiplier}
              editConfigJSON={editConfigJSON}
              setEditConfigJSON={setEditConfigJSON}
              applyChannelPatch={applyChannelPatch}
            />
          </>
        )}
      </BootstrapModal>
    </div>
  );
}
