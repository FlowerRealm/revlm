import { useCallback, useEffect, useMemo, useRef, useState, type CSSProperties, type ReactNode } from 'react';

import {
  DndContext,
  MouseSensor,
  TouchSensor,
  closestCenter,
  pointerWithin,
  useSensor,
  useSensors,
  type CollisionDetection,
  type DragEndEvent,
  type DragStartEvent,
  type Modifier,
} from '@dnd-kit/core';
import { SortableContext, arrayMove, useSortable, verticalListSortingStrategy } from '@dnd-kit/sortable';
import { CSS } from '@dnd-kit/utilities';

import {
  createUserToken,
  deleteUserToken,
  getUserTokenChannelGroups,
  getUserTokenModelMappings,
  listUserTokens,
  replaceUserTokenChannelGroups,
  replaceUserTokenModelMappings,
  revealUserToken,
  revokeUserToken,
  rotateUserToken,
  type UserTokenChannelGroups,
  type TokenModelMapping,
  type TokenModelTargetOption,
  type UserToken,
} from '../api/tokens';
import { getUsageWindows, type UsageWindow } from '../api/usage';
import { BootstrapModal } from '../components/BootstrapModal';
import { DividedStack } from '../components/DividedStack';
import { SegmentedFrame } from '../components/SegmentedFrame';
import { closeModalById } from '../components/modal';
import { PortalDragOverlay } from '../components/PortalDragOverlay';
import { formatUSDPlain } from '../format/money';
import { cacheHitRate, formatLocalDate, formatLocalDateTimeMinute } from './usage/usageUtils';

type UseSortableReturn = ReturnType<typeof useSortable>;
type SortableRowRenderArgs = Pick<
  UseSortableReturn,
  | 'attributes'
  | 'listeners'
  | 'setActivatorNodeRef'
  | 'setNodeRef'
  | 'transform'
  | 'transition'
  | 'isDragging'
  | 'isOver'
> & {
  setRowRef: (node: HTMLTableRowElement | null) => void;
};

type TokenChannelGroupRow = {
  id: number;
  name: string;
  status: number;
  price_multiplier: string;
  description?: string | null;
};

type TokenModelMappingRow = TokenModelMapping & {
  rowKey: string;
};

function wrapDndListeners(listeners: SortableRowRenderArgs['listeners']): SortableRowRenderArgs['listeners'] {
  if (!listeners) return listeners;

  const getTarget = (e: unknown): Element | null => {
    if (!e || typeof e !== 'object') return null;
    const target = (e as { target?: unknown }).target;
    return target instanceof Element ? target : null;
  };

  const shouldIgnore = (target: Element | null) => {
    if (!target) return false;
    return !!target.closest('button, a, input, textarea, select, label, [data-rlm-dnd-ignore]');
  };

  const wrap = (fn: unknown) => {
    if (typeof fn !== 'function') return fn;
    return (e: unknown) => {
      if (shouldIgnore(getTarget(e))) return;
      (fn as (e: unknown) => void)(e);
    };
  };

  const base = listeners as unknown as Record<string, unknown>;
  const out: Record<string, unknown> = { ...base };
  out.onMouseDown = wrap(base.onMouseDown);
  out.onTouchStart = wrap(base.onTouchStart);
  out.onPointerDown = wrap(base.onPointerDown);
  return out as unknown as SortableRowRenderArgs['listeners'];
}

const restrictToVerticalAxisModifier: Modifier = ({ transform }) => {
  if (!transform) return transform;
  return { ...transform, x: 0 };
};

function SortableRow({
  id,
  disabled,
  children,
}: {
  id: number;
  disabled: boolean | { draggable?: boolean; droppable?: boolean };
  children: (args: SortableRowRenderArgs) => ReactNode;
}) {
  const { attributes, listeners, setActivatorNodeRef, setNodeRef, transform, transition, isDragging, isOver } =
    useSortable({ id, disabled });
  const wrappedListeners = useMemo(() => wrapDndListeners(listeners), [listeners]);
  const setRowRef = useCallback(
    (node: HTMLTableRowElement | null) => {
      setNodeRef(node);
      setActivatorNodeRef(node);
    },
    [setActivatorNodeRef, setNodeRef]
  );
  return children({
    attributes,
    listeners: wrappedListeners,
    setActivatorNodeRef,
    setNodeRef,
    setRowRef,
    transform,
    transition,
    isDragging,
    isOver,
  });
}

export function TokensPage() {
  const [tokens, setTokens] = useState<UserToken[]>([]);
  const [tokensLoading, setTokensLoading] = useState(true);
  const [tokensErr, setTokensErr] = useState('');
  const [revealed, setRevealed] = useState<Record<number, string>>({});
  const [revealLoading, setRevealLoading] = useState<Record<number, boolean>>({});
  const [copiedID, setCopiedID] = useState<number | null>(null);

  const [name, setName] = useState('');

  const openGeneratedTokenModalBtnRef = useRef<HTMLButtonElement | null>(null);
  const pendingGeneratedTokenRef = useRef<string | null>(null);
  const [generatedToken, setGeneratedToken] = useState('');
  const [generatedCopied, setGeneratedCopied] = useState(false);

  const openTokenGroupsModalBtnRef = useRef<HTMLButtonElement | null>(null);
  const [tokenGroupsToken, setTokenGroupsToken] = useState<UserToken | null>(null);
  const [tokenGroupsData, setTokenGroupsData] = useState<UserTokenChannelGroups | null>(null);
  const [loading, setLoading] = useState(false);
  const [reordering, setReordering] = useState(false);
  const [err, setErr] = useState('');
  const [notice, setNotice] = useState('');
  const [channels, setChannels] = useState<TokenChannelGroupRow[]>([]);
  const [addGroup, setAddGroup] = useState('');
  const [draggingID, setDraggingID] = useState<number | null>(null);
  const [dragOverlayWidth, setDragOverlayWidth] = useState<number | null>(null);
  const [dragOverlayColWidths, setDragOverlayColWidths] = useState<number[] | null>(null);

  const openTokenUsageModalBtnRef = useRef<HTMLButtonElement | null>(null);
  const [usageToken, setUsageToken] = useState<UserToken | null>(null);
  const [usageWindow, setUsageWindow] = useState<UsageWindow | null>(null);
  const [usageStart, setUsageStart] = useState('');
  const [usageEnd, setUsageEnd] = useState('');
  const [usageLoading, setUsageLoading] = useState(false);
  const [usageErr, setUsageErr] = useState('');

  const openModelMappingsModalBtnRef = useRef<HTMLButtonElement | null>(null);
  const [modelMappingsToken, setModelMappingsToken] = useState<UserToken | null>(null);
  const [modelMappings, setModelMappings] = useState<TokenModelMappingRow[]>([]);
  const [modelTargets, setModelTargets] = useState<TokenModelTargetOption[]>([]);
  const [modelMappingsLoaded, setModelMappingsLoaded] = useState(false);
  const [modelMappingsLoading, setModelMappingsLoading] = useState(false);
  const [modelMappingsSaving, setModelMappingsSaving] = useState(false);
  const [modelMappingsErr, setModelMappingsErr] = useState('');
  const [modelMappingsNotice, setModelMappingsNotice] = useState('');

  const channelGroupIDByNameRef = useRef<Map<string, number>>(new Map());
  const nextChannelGroupIDRef = useRef(1);
  const nextModelMappingRowKeyRef = useRef(1);

  const getChannelGroupID = useCallback((rawName: string): number => {
    const name = (rawName || '').trim();
    if (!name) return 0;
    const existing = channelGroupIDByNameRef.current.get(name);
    if (existing) return existing;
    const nextID = nextChannelGroupIDRef.current++;
    channelGroupIDByNameRef.current.set(name, nextID);
    return nextID;
  }, []);

  useEffect(() => {
    if (copiedID == null) return;
    const t = window.setTimeout(() => setCopiedID(null), 2000);
    return () => window.clearTimeout(t);
  }, [copiedID]);

  useEffect(() => {
    if (!generatedCopied) return;
    const t = window.setTimeout(() => setGeneratedCopied(false), 2000);
    return () => window.clearTimeout(t);
  }, [generatedCopied]);

  async function refresh() {
    setTokensErr('');
    setTokensLoading(true);
    try {
      const res = await listUserTokens();
      if (!res.success) {
        throw new Error(res.message || '加载失败');
      }
      const nextTokens = res.data || [];
      setTokens(nextTokens);
      setRevealed((prev) => {
        const active = new Set(nextTokens.filter((t) => t.status === 1).map((t) => t.id));
        const next: Record<number, string> = {};
        for (const [k, v] of Object.entries(prev)) {
          const id = Number(k);
          if (active.has(id)) next[id] = v;
        }
        return next;
      });
      setRevealLoading((prev) => {
        const active = new Set(nextTokens.filter((t) => t.status === 1).map((t) => t.id));
        const next: Record<number, boolean> = {};
        for (const [k, v] of Object.entries(prev)) {
          const id = Number(k);
          if (active.has(id)) next[id] = v;
        }
        return next;
      });
    } catch (e) {
      setTokensErr(e instanceof Error ? e.message : '加载失败');
    } finally {
      setTokensLoading(false);
    }
  }

  function openGeneratedTokenModal(tok: string) {
    setGeneratedCopied(false);
    setGeneratedToken(tok);
    window.setTimeout(() => openGeneratedTokenModalBtnRef.current?.click(), 0);
  }

  async function copyText(raw: string): Promise<boolean> {
    try {
      await navigator.clipboard.writeText(raw);
      return true;
    } catch {
      // fallback
    }
    try {
      const el = document.createElement('textarea');
      el.value = raw;
      el.setAttribute('readonly', 'true');
      el.style.position = 'fixed';
      el.style.top = '0';
      el.style.left = '0';
      el.style.opacity = '0';
      document.body.appendChild(el);
      el.select();
      const ok = document.execCommand('copy');
      document.body.removeChild(el);
      return ok;
    } catch {
      return false;
    }
  }

  async function copyToken(raw: string, tokenID: number) {
    const ok = await copyText(raw);
    if (ok) setCopiedID(tokenID);
  }

  async function revealToken(tokenID: number): Promise<string> {
    if (revealed[tokenID]) return revealed[tokenID];
    setRevealLoading((prev) => ({ ...prev, [tokenID]: true }));
    try {
      const res = await revealUserToken(tokenID);
      if (!res.success) {
        throw new Error(res.message || '查看失败');
      }
      const tok = (res.data?.token || '').toString();
      if (tok.trim() === '') {
        throw new Error('查看失败');
      }
      setRevealed((prev) => ({ ...prev, [tokenID]: tok }));
      return tok;
    } finally {
      setRevealLoading((prev) => ({ ...prev, [tokenID]: false }));
    }
  }

  function normalizeGroupOrder(inGroups: string[]): string[] {
    const out: string[] = [];
    const seen = new Set<string>();
    for (const raw of inGroups || []) {
      const name = (raw || '').trim();
      if (!name) continue;
      if (seen.has(name)) continue;
      seen.add(name);
      out.push(name);
    }
    return out;
  }

  function normalizeChannelGroupOrder(list: TokenChannelGroupRow[]): TokenChannelGroupRow[] {
    const out: TokenChannelGroupRow[] = [];
    const seen = new Set<string>();
    for (const raw of list || []) {
      const name = (raw?.name || '').trim();
      if (!name) continue;
      if (seen.has(name)) continue;
      seen.add(name);
      out.push({ ...raw, name });
    }
    return out;
  }

  function normalizeChannelGroupSections(list: TokenChannelGroupRow[]): TokenChannelGroupRow[] {
    const normalized = normalizeChannelGroupOrder(list);
    const enabled = normalized.filter((ch) => ch.status === 1);
    const disabled = normalized.filter((ch) => ch.status !== 1);
    return [...enabled, ...disabled];
  }

  function buildChannelGroupRows(data: UserTokenChannelGroups | null | undefined): TokenChannelGroupRow[] {
    const optionByName = new Map<string, UserTokenChannelGroups['allowed_channel_groups'][number]>();
    for (const option of data?.allowed_channel_groups || []) {
      const name = (option.name || '').trim();
      if (!name) continue;
      optionByName.set(name, option);
    }
    const rows: TokenChannelGroupRow[] = normalizeGroupOrder(
      (data?.bindings || []).map((x) => (x.channel_group_name || '').trim()).filter((x) => x)
    ).map((name) => {
      const option = optionByName.get(name);
      return {
        id: getChannelGroupID(name),
        name,
        status: option?.status ?? 0,
        price_multiplier: option?.price_multiplier || '1',
        description: option?.description || null,
      };
    });
    return normalizeChannelGroupSections(rows);
  }

  function addSelectedGroup(name: string) {
    const v = (name || '').trim();
    if (!v) return;
    setChannels((prev) => {
      if (prev.some((x) => x.name === v)) return prev;
      const option = (tokenGroupsData?.allowed_channel_groups || []).find((g) => g.name === v);
      const status = option?.status ?? 1;
      const row: TokenChannelGroupRow = {
        id: getChannelGroupID(v),
        name: v,
        status,
        price_multiplier: option?.price_multiplier || '1',
        description: option?.description || null,
      };
      return normalizeChannelGroupSections([...prev, row]);
    });
  }

  function removeSelectedGroup(name: string) {
    const v = (name || '').trim();
    if (!v) return;
    setChannels((prev) => normalizeChannelGroupSections(prev.filter((x) => x.name !== v)));
  }

  const reduceMotion = useMemo(() => {
    if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') return false;
    return window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  }, []);

  const sensors = useSensors(
    useSensor(MouseSensor, { activationConstraint: { distance: 6 } }),
    useSensor(TouchSensor, { activationConstraint: { delay: 180, tolerance: 7 } })
  );
  const collisionDetection = useCallback<CollisionDetection>((args) => {
    const pointerCollisions = pointerWithin(args);
    if (pointerCollisions.length > 0) return pointerCollisions;
    return closestCenter(args);
  }, []);

  const enabledChannelIDs = useMemo(() => channels.filter((ch) => ch.status === 1).map((ch) => ch.id), [channels]);
  const selectedGroupNameSet = useMemo(() => new Set(channels.map((ch) => ch.name)), [channels]);
  const modelTargetIDSet = useMemo(
    () => new Set(modelTargets.map((target) => (target.public_id || '').trim()).filter((x) => x)),
    [modelTargets]
  );
  const dragOverlayChannel = useMemo(() => {
    if (draggingID === null) return null;
    return channels.find((ch) => ch.id === draggingID) || null;
  }, [channels, draggingID]);

  async function saveTokenGroups() {
    const tokenID = tokenGroupsToken?.id || 0;
    if (!tokenID) {
      setErr('未选择 Token');
      setNotice('');
      return;
    }
    const groupNames = normalizeChannelGroupSections(channels).map((ch) => ch.name);
    if (!groupNames.length) {
      setErr('至少选择 1 个渠道组');
      setNotice('');
      return;
    }
    setErr('');
    setNotice('');
    setReordering(true);
    try {
      const res = await replaceUserTokenChannelGroups(tokenID, groupNames);
      if (!res.success) throw new Error(res.message || '保存失败');
      const refreshed = await getUserTokenChannelGroups(tokenID);
      if (refreshed.success) {
        const d = refreshed.data || null;
        setTokenGroupsData(d);
        setChannels(buildChannelGroupRows(d));
      }
      setNotice('已保存');
    } catch (e) {
      setErr(e instanceof Error ? e.message : '保存失败');
    } finally {
      setReordering(false);
    }
  }

  useEffect(() => {
    if (!draggingID) return;
    const prevCursor = document.body.style.cursor;
    const prevUserSelect = document.body.style.userSelect;
    document.body.style.cursor = 'grabbing';
    document.body.style.userSelect = 'none';
    document.body.classList.add('rlm-dnd-sorting');
    return () => {
      document.body.style.cursor = prevCursor;
      document.body.style.userSelect = prevUserSelect;
      document.body.classList.remove('rlm-dnd-sorting');
    };
  }, [draggingID]);

  function sameIDOrder(a: number[], b: number[]): boolean {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
      if (a[i] !== b[i]) return false;
    }
    return true;
  }

  function handleDragStart(e: DragStartEvent) {
    if (loading || reordering) return;
    const raw = e.active.id;
    const channelID = typeof raw === 'number' ? raw : Number.parseInt(String(raw), 10);
    if (!Number.isFinite(channelID) || channelID <= 0) return;
    setDraggingID(channelID);

    const row =
      typeof document !== 'undefined'
        ? (document.querySelector(
            `tr[data-rlm-channel-row="main"][data-rlm-channel-id="${channelID}"]`
          ) as HTMLElement | null)
        : null;
    const rect = row?.getBoundingClientRect() || null;
    setDragOverlayWidth(rect ? Math.round(rect.width) : null);

    const cols = row ? Array.from(row.querySelectorAll('td')) : [];
    const colWidths = cols.map((td) => Math.round(td.getBoundingClientRect().width));
    setDragOverlayColWidths(
      colWidths.length > 0 && colWidths.every((w) => Number.isFinite(w) && w > 0) ? colWidths : null
    );
  }

  function handleDragCancel() {
    setDraggingID(null);
    setDragOverlayWidth(null);
    setDragOverlayColWidths(null);
  }

  function handleDragEnd(e: DragEndEvent) {
    const rawActive = e.active.id;
    const overRaw = e.over?.id;
    const activeID = typeof rawActive === 'number' ? rawActive : Number.parseInt(String(rawActive), 10);
    const overID =
      overRaw == null ? null : typeof overRaw === 'number' ? overRaw : Number.parseInt(String(overRaw), 10);

    setDraggingID(null);
    setDragOverlayWidth(null);
    setDragOverlayColWidths(null);

    if (!overID || !Number.isFinite(activeID) || activeID <= 0) return;
    if (activeID === overID) return;

    const startList = channels;
    const enabled = startList.filter((ch) => ch.status === 1);
    const disabled = startList.filter((ch) => ch.status !== 1);
    const from = enabled.findIndex((c) => c.id === activeID);
    const to = enabled.findIndex((c) => c.id === overID);
    if (from < 0 || to < 0) return;

    const nextEnabled = arrayMove(enabled, from, to);
    const next = [...nextEnabled, ...disabled];
    const nextIDs = next.map((c) => c.id);
    if (
      sameIDOrder(
        nextIDs,
        startList.map((c) => c.id)
      )
    )
      return;

    setChannels(next);
  }

  async function refreshUsage(tokenID: number, override?: { start?: string; end?: string }) {
    setUsageErr('');
    setUsageLoading(true);
    try {
      const startValue = (override?.start ?? usageStart).trim();
      const endValue = (override?.end ?? usageEnd).trim();
      const res = await getUsageWindows(startValue || undefined, endValue || undefined, tokenID);
      if (!res.success) throw new Error(res.message || '加载失败');
      const window0 = res.data?.windows?.[0] ?? null;
      setUsageWindow(window0);

      const day0 = window0 ? formatLocalDate(String(window0.since)) : '';
      if (window0) {
        if (!startValue && day0) setUsageStart(day0);
        if (!endValue && (startValue || day0)) setUsageEnd(startValue || day0);
      }
    } catch (e) {
      setUsageErr(e instanceof Error ? e.message : '加载失败');
      setUsageWindow(null);
    } finally {
      setUsageLoading(false);
    }
  }

  async function openTokenUsageModal(t: UserToken) {
    setTokensErr('');
    setUsageToken(t);
    setUsageWindow(null);
    setUsageStart('');
    setUsageEnd('');
    setUsageErr('');
    window.setTimeout(() => openTokenUsageModalBtnRef.current?.click(), 0);
    void refreshUsage(t.id, { start: '', end: '' });
  }

  async function openTokenGroupsModal(t: UserToken) {
    setTokensErr('');
    setErr('');
    setNotice('');
    setTokenGroupsToken(t);
    setTokenGroupsData(null);
    setLoading(true);
    setReordering(false);
    setAddGroup('');
    setChannels([]);
    try {
      const res = await getUserTokenChannelGroups(t.id);
      if (!res.success) throw new Error(res.message || '加载失败');
      const d = res.data || null;
      setTokenGroupsData(d);
      setChannels(buildChannelGroupRows(d));
    } catch (e) {
      const msg = e instanceof Error ? e.message : '加载失败';
      setErr(msg);
      setChannels([]);
    } finally {
      setLoading(false);
      window.setTimeout(() => openTokenGroupsModalBtnRef.current?.click(), 0);
    }
  }

  function makeModelMappingRow(row: TokenModelMapping): TokenModelMappingRow {
    const input = (row.input_model || '').trim();
    const target = (row.target_model || '').trim();
    return {
      input_model: input,
      target_model: target,
      rowKey: `model-mapping:${nextModelMappingRowKeyRef.current++}`,
    };
  }

  function normalizeModelMappings(rows: TokenModelMapping[]): TokenModelMappingRow[] {
    const out: TokenModelMappingRow[] = [];
    for (const row of rows || []) {
      const input = (row.input_model || '').trim();
      const target = (row.target_model || '').trim();
      if (!input && !target) continue;
      out.push(makeModelMappingRow({ input_model: input, target_model: target }));
    }
    return out;
  }

  function validateModelMappings(rows: TokenModelMapping[], allowedTargets: Set<string>) {
    const seen = new Set<string>();
    for (const row of rows) {
      if (!row.input_model || !row.target_model) return '输入模型和目标模型都不能为空';
      if (seen.has(row.input_model)) return `输入模型重复: ${row.input_model}`;
      seen.add(row.input_model);
      if (!allowedTargets.has(row.target_model)) return `目标模型不可用: ${row.target_model}`;
    }
    return '';
  }

  async function openModelMappingsModal(t: UserToken) {
    setTokensErr('');
    setModelMappingsToken(t);
    setModelMappings([]);
    setModelTargets([]);
    setModelMappingsLoaded(false);
    setModelMappingsErr('');
    setModelMappingsNotice('');
    setModelMappingsLoading(true);
    setModelMappingsSaving(false);
    window.setTimeout(() => openModelMappingsModalBtnRef.current?.click(), 0);
    try {
      const res = await getUserTokenModelMappings(t.id);
      if (!res.success) throw new Error(res.message || '加载失败');
      setModelTargets(res.data?.available_target_models || []);
      setModelMappings(normalizeModelMappings(res.data?.mappings || []));
      setModelMappingsLoaded(true);
    } catch (e) {
      setModelMappingsErr(e instanceof Error ? e.message : '加载失败');
    } finally {
      setModelMappingsLoading(false);
    }
  }

  async function saveModelMappings() {
    if (!modelMappingsToken) return;
    setModelMappingsErr('');
    setModelMappingsNotice('');
    if (!modelMappingsLoaded) {
      setModelMappingsErr('模型映射详情尚未加载完成');
      return;
    }
    const normalized = normalizeModelMappings(modelMappings);
    const validationError = validateModelMappings(normalized, modelTargetIDSet);
    if (validationError) {
      setModelMappingsErr(validationError);
      return;
    }
    const payload = normalized.map(({ input_model, target_model }) => ({ input_model, target_model }));
    setModelMappingsSaving(true);
    try {
      const res = await replaceUserTokenModelMappings(modelMappingsToken.id, payload);
      if (!res.success) throw new Error(res.message || '保存失败');
      const refreshed = await getUserTokenModelMappings(modelMappingsToken.id);
      if (refreshed.success) {
        setModelTargets(refreshed.data?.available_target_models || []);
        setModelMappings(normalizeModelMappings(refreshed.data?.mappings || []));
      } else {
        setModelMappings(normalized);
      }
      setModelMappingsNotice('已保存');
    } catch (e) {
      setModelMappingsErr(e instanceof Error ? e.message : '保存失败');
    } finally {
      setModelMappingsSaving(false);
    }
  }

  useEffect(() => {
    void refresh();
  }, []);

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <DividedStack>
          <div className="card mb-0">
            <div className="card-body d-flex flex-column flex-md-row justify-content-between align-items-center">
              <div className="d-flex align-items-center mb-3 mb-md-0">
                <div
                  className="bg-primary bg-opacity-10 text-primary rounded-circle d-flex align-items-center justify-content-center me-3"
                  style={{ width: 48, height: 48 }}
                >
                  <span className="fs-4 material-symbols-rounded">key</span>
                </div>
                <div>
                  <h5 className="mb-1 fw-semibold">我的 API 令牌</h5>
                  <p className="mb-0 text-muted small">
                    为安全起见，令牌默认隐藏；可在此页查看/复制。令牌撤销后无法查看。
                  </p>
                </div>
              </div>
              <button
                type="button"
                className="btn btn-primary btn-sm"
                data-bs-toggle="modal"
                data-bs-target="#createTokenModal"
              >
                <span className="me-1 material-symbols-rounded">add</span> 创建令牌
              </button>
            </div>
          </div>

          {tokensErr ? (
            <div className="alert alert-danger mb-0" role="alert">
              <span className="me-2 material-symbols-rounded">report</span>
              {tokensErr}
            </div>
          ) : null}

          <div className="card h-100 overflow-hidden mb-0">
            <div className="card-body p-0">
              <div className="table-responsive">
                <table className="table table-hover align-middle mb-0">
                  <thead className="bg-light text-muted small text-uppercase">
                    <tr>
                      <th scope="col" className="fw-medium ps-4 py-3">
                        名称
                      </th>
                      <th scope="col" className="fw-medium py-3">
                        预览
                      </th>
                      <th scope="col" className="fw-medium py-3">
                        状态
                      </th>
                      <th scope="col" className="fw-medium text-end pe-4 py-3">
                        操作
                      </th>
                    </tr>
                  </thead>
                  <tbody className="border-top-0">
                    {tokensLoading ? (
                      <tr>
                        <td colSpan={4} className="text-center py-5 text-muted">
                          加载中…
                        </td>
                      </tr>
                    ) : tokens.length === 0 ? (
                      <tr>
                        <td colSpan={4} className="text-center py-5 text-muted">
                          <div className="mb-2">
                            <span className="fs-3 text-light-emphasis material-symbols-rounded">inbox</span>
                          </div>
                          暂无令牌，点击右上角按钮创建一个。
                        </td>
                      </tr>
                    ) : (
                      tokens.map((t) => (
                        <tr key={t.id}>
                          <td className="ps-4 py-3">
                            {t.name ? (
                              <span className="fw-medium text-dark">{t.name}</span>
                            ) : (
                              <span className="text-muted small fst-italic">无备注</span>
                            )}
                          </td>
                          <td className="py-3">
                            {revealed[t.id] ? (
                              <code className="bg-light px-2 py-1 rounded text-dark border user-select-all">
                                {revealed[t.id]}
                              </code>
                            ) : (
                              <span className="text-muted small">-</span>
                            )}
                          </td>
                          <td className="py-3">
                            {t.status === 1 ? (
                              <span className="badge bg-success bg-opacity-10 text-success rounded-pill px-2">
                                活跃
                              </span>
                            ) : (
                              <span className="badge bg-secondary bg-opacity-10 text-secondary rounded-pill px-2">
                                已撤销
                              </span>
                            )}
                          </td>
                          <td className="text-end pe-4 py-3">
                            {t.status === 1 ? (
                              <>
                                <button
                                  className="btn btn-link text-secondary p-0 text-decoration-none small"
                                  type="button"
                                  disabled={tokensLoading || revealLoading[t.id]}
                                  onClick={async () => {
                                    setTokensErr('');
                                    if (revealed[t.id]) {
                                      setRevealed((prev) => {
                                        const next = { ...prev };
                                        delete next[t.id];
                                        return next;
                                      });
                                      return;
                                    }
                                    try {
                                      await revealToken(t.id);
                                    } catch (e) {
                                      setTokensErr(e instanceof Error ? e.message : '查看失败');
                                    }
                                  }}
                                >
                                  {revealed[t.id] ? '隐藏' : '查看'}
                                </button>

                                <span className="text-muted small mx-2">|</span>

                                <button
                                  className="btn btn-link text-secondary p-0 text-decoration-none small"
                                  type="button"
                                  disabled={tokensLoading || revealLoading[t.id]}
                                  onClick={async () => {
                                    setTokensErr('');
                                    try {
                                      const tok = revealed[t.id] ? revealed[t.id] : await revealToken(t.id);
                                      await copyToken(tok, t.id);
                                    } catch (e) {
                                      setTokensErr(e instanceof Error ? e.message : '复制失败');
                                    }
                                  }}
                                >
                                  {copiedID === t.id ? '已复制' : '复制'}
                                </button>

                                <span className="text-muted small mx-2">|</span>

                                <button
                                  className="btn btn-link text-secondary p-0 text-decoration-none small"
                                  type="button"
                                  disabled={tokensLoading}
                                  onClick={() => void openTokenGroupsModal(t)}
                                >
                                  渠道组
                                </button>

                                <span className="text-muted small mx-2">|</span>

                                <button
                                  className="btn btn-link text-secondary p-0 text-decoration-none small"
                                  type="button"
                                  disabled={tokensLoading}
                                  onClick={() => void openModelMappingsModal(t)}
                                >
                                  模型映射
                                </button>

                                <span className="text-muted small mx-2">|</span>

                                <button
                                  className="btn btn-link text-secondary p-0 text-decoration-none small"
                                  type="button"
                                  disabled={tokensLoading}
                                  onClick={() => void openTokenUsageModal(t)}
                                >
                                  用量
                                </button>

                                <span className="text-muted small mx-2">|</span>
                              </>
                            ) : null}

                            <button
                              className="btn btn-link text-primary p-0 text-decoration-none small"
                              type="button"
                              disabled={tokensLoading}
                              onClick={async () => {
                                setTokensErr('');
                                setRevealed((prev) => {
                                  const next = { ...prev };
                                  delete next[t.id];
                                  return next;
                                });
                                try {
                                  const res = await rotateUserToken(t.id);
                                  if (!res.success) {
                                    throw new Error(res.message || '重新生成失败');
                                  }
                                  const tok = res.data?.token;
                                  await refresh();
                                  if (tok) openGeneratedTokenModal(tok);
                                } catch (e) {
                                  setTokensErr(e instanceof Error ? e.message : '重新生成失败');
                                }
                              }}
                            >
                              重新生成
                            </button>

                            <span className="text-muted small mx-2">|</span>

                            {t.status === 1 ? (
                              <button
                                className="btn btn-link text-danger p-0 text-decoration-none small"
                                type="button"
                                disabled={tokensLoading}
                                onClick={async () => {
                                  setTokensErr('');
                                  try {
                                    const res = await revokeUserToken(t.id);
                                    if (!res.success) {
                                      throw new Error(res.message || '撤销失败');
                                    }
                                    await refresh();
                                  } catch (e) {
                                    setTokensErr(e instanceof Error ? e.message : '撤销失败');
                                  }
                                }}
                              >
                                撤销
                              </button>
                            ) : (
                              <button
                                className="btn btn-link text-danger p-0 text-decoration-none small"
                                type="button"
                                disabled={tokensLoading}
                                onClick={async () => {
                                  setTokensErr('');
                                  try {
                                    const res = await deleteUserToken(t.id);
                                    if (!res.success) {
                                      throw new Error(res.message || '删除失败');
                                    }
                                    await refresh();
                                  } catch (e) {
                                    setTokensErr(e instanceof Error ? e.message : '删除失败');
                                  }
                                }}
                              >
                                删除
                              </button>
                            )}
                          </td>
                        </tr>
                      ))
                    )}
                  </tbody>
                </table>
              </div>
            </div>
          </div>
        </DividedStack>
      </SegmentedFrame>

      {/* programmatically open the generated-token modal */}
      <button
        ref={openGeneratedTokenModalBtnRef}
        type="button"
        className="d-none"
        data-bs-toggle="modal"
        data-bs-target="#generatedTokenModal"
      ></button>

      {/* programmatically open the token-groups modal */}
      <button
        ref={openTokenGroupsModalBtnRef}
        type="button"
        className="d-none"
        data-bs-toggle="modal"
        data-bs-target="#tokenGroupsModal"
      ></button>

      {/* programmatically open the token-usage modal */}
      <button
        ref={openTokenUsageModalBtnRef}
        type="button"
        className="d-none"
        data-bs-toggle="modal"
        data-bs-target="#tokenUsageModal"
      ></button>

      {/* programmatically open the model-mappings modal */}
      <button
        ref={openModelMappingsModalBtnRef}
        type="button"
        className="d-none"
        data-bs-toggle="modal"
        data-bs-target="#modelMappingsModal"
      ></button>

      <BootstrapModal
        id="tokenUsageModal"
        title="令牌用量"
        dialogClassName="modal-dialog-centered modal-lg"
        onHidden={() => {
          setUsageToken(null);
          setUsageWindow(null);
          setUsageStart('');
          setUsageEnd('');
          setUsageLoading(false);
          setUsageErr('');
        }}
      >
        {usageToken ? (
          <div>
            <div className="mb-3">
              <div className="text-muted small mb-1">令牌</div>
              <div className="fw-semibold">{(usageToken.name || `Token #${usageToken.id}`).toString()}</div>
            </div>

            {usageErr ? (
              <div className="alert alert-danger mb-3" role="alert">
                <span className="me-2 material-symbols-rounded">warning</span>
                {usageErr}
              </div>
            ) : null}

            <form
              className="row g-2 align-items-end mb-3"
              onSubmit={(e) => {
                e.preventDefault();
                void refreshUsage(usageToken.id);
              }}
            >
              <div className="col-auto">
                <label className="form-label small text-muted mb-1">开始日期</label>
                <input
                  className="form-control form-control-sm"
                  type="date"
                  value={usageStart}
                  onChange={(e) => setUsageStart(e.target.value)}
                  disabled={usageLoading}
                />
              </div>
              <div className="col-auto">
                <label className="form-label small text-muted mb-1">结束日期</label>
                <input
                  className="form-control form-control-sm"
                  type="date"
                  value={usageEnd}
                  onChange={(e) => setUsageEnd(e.target.value)}
                  disabled={usageLoading}
                />
              </div>
              <div className="col-auto d-flex gap-2">
                <button className="btn btn-sm btn-primary" type="submit" disabled={usageLoading}>
                  查询
                </button>
                <button
                  className="btn btn-sm btn-white border text-dark"
                  type="button"
                  disabled={usageLoading}
                  onClick={() => {
                    setUsageStart('');
                    setUsageEnd('');
                    void refreshUsage(usageToken.id, { start: '', end: '' });
                  }}
                >
                  重置
                </button>
              </div>
            </form>

            {usageLoading ? <div className="text-muted">加载中…</div> : null}

            {usageWindow ? (
              <div className="table-responsive">
                <table className="table table-sm mb-0">
                  <tbody>
                    <tr>
                      <td className="text-muted">时间范围</td>
                      <td className="text-end">
                        {formatLocalDateTimeMinute(String(usageWindow.since))} -{' '}
                        {formatLocalDateTimeMinute(String(usageWindow.until))}
                      </td>
                    </tr>
                    <tr>
                      <td className="text-muted">消耗 (USD)</td>
                      <td className="text-end">{formatUSDPlain(usageWindow.committed_usd)}</td>
                    </tr>
                    <tr>
                      <td className="text-muted">请求数</td>
                      <td className="text-end">{usageWindow.requests}</td>
                    </tr>
                    <tr>
                      <td className="text-muted">Tokens</td>
                      <td className="text-end">{usageWindow.tokens}</td>
                    </tr>
                    <tr>
                      <td className="text-muted">Tokens</td>
                      <td className="text-end">
                        {usageWindow.input_tokens}/{usageWindow.output_tokens}/
                        {usageWindow.cache_read_tokens + usageWindow.cache_creation_tokens}
                      </td>
                    </tr>
                    <tr>
                      <td className="text-muted">缓存率</td>
                      <td className="text-end">{cacheHitRate(usageWindow.cache_ratio)}</td>
                    </tr>
                    <tr>
                      <td className="text-muted">RPM/TPM</td>
                      <td className="text-end">
                        {usageWindow.rpm}/{usageWindow.tpm}
                      </td>
                    </tr>
                  </tbody>
                </table>
              </div>
            ) : null}
          </div>
        ) : (
          <div className="text-muted">请选择一个令牌。</div>
        )}
      </BootstrapModal>

      <BootstrapModal
        id="generatedTokenModal"
        title="令牌已生成"
        dialogClassName="modal-dialog-centered"
        onHidden={() => {
          setGeneratedCopied(false);
          setGeneratedToken('');
        }}
      >
        <div className="alert alert-warning border-0 bg-warning bg-opacity-10 d-flex align-items-start mb-3">
          <span className="me-2 mt-1 material-symbols-rounded">warning</span>
          <div className="small">请复制并妥善保存。也可以在令牌列表页查看/复制（默认隐藏）。令牌撤销后无法查看。</div>
        </div>

        <div className="mb-3">
          <label className="form-label small fw-bold text-uppercase text-muted">API 令牌</label>
          <div className="input-group input-group-lg">
            <input
              type="text"
              className="form-control font-monospace bg-light border-end-0"
              value={generatedToken}
              readOnly
              onClick={(e) => {
                try {
                  e.currentTarget.select();
                } catch {
                  // ignore
                }
              }}
            />
            <button
              className={`btn ${generatedCopied ? 'btn-success text-white' : 'btn-light'} border border-start-0 px-4`}
              type="button"
              title="点击复制"
              disabled={generatedToken.trim() === ''}
              onClick={async () => {
                setTokensErr('');
                const ok = await copyText(generatedToken);
                if (!ok) {
                  setTokensErr('复制失败');
                  return;
                }
                setGeneratedCopied(true);
              }}
            >
              <span className="material-symbols-rounded">{generatedCopied ? 'check' : 'content_copy'}</span>
            </button>
          </div>
          <div
            className={`text-success small mt-2 opacity-0 transition-opacity${generatedCopied ? ' opacity-100' : ''}`}
          >
            <span className="me-1 material-symbols-rounded">check</span>已成功复制到剪贴板
          </div>
        </div>

        <div className="modal-footer border-top-0 px-0 pb-0">
          <button type="button" className="btn btn-light" data-bs-dismiss="modal">
            关闭
          </button>
        </div>
      </BootstrapModal>

      <BootstrapModal
        id="tokenGroupsModal"
        title={
          tokenGroupsToken
            ? `渠道组：${(tokenGroupsToken.name || '').trim() || `Token #${tokenGroupsToken.id}`}`
            : '渠道组'
        }
        dialogClassName="modal-dialog-centered modal-lg"
        onHidden={() => {
          setTokenGroupsToken(null);
          setTokenGroupsData(null);
          setErr('');
          setNotice('');
          setLoading(false);
          setReordering(false);
          setChannels([]);
          setAddGroup('');
          setDraggingID(null);
          setDragOverlayWidth(null);
          setDragOverlayColWidths(null);
        }}
      >
        {!tokenGroupsToken ? (
          <div className="text-muted">未选择 Token。</div>
        ) : (
          <div>
            {err ? (
              <div className="alert alert-danger d-flex align-items-center" role="alert">
                <span className="me-2 material-symbols-rounded">warning</span>
                <div>{err}</div>
              </div>
            ) : null}

            {notice ? (
              <div className="alert alert-success d-flex align-items-center" role="alert">
                <span className="me-2 material-symbols-rounded">check_circle</span>
                <div>{notice}</div>
              </div>
            ) : null}

            <div className="d-flex flex-wrap gap-2 align-items-center mb-3 small">
              <span className="badge bg-light text-dark border">
                生效顺序:{' '}
                <span className="font-monospace">
                  {(tokenGroupsData?.effective_bindings || [])
                    .map((b) => b.channel_group_name)
                    .filter((x) => x)
                    .join(' → ') || '-'}
                </span>
              </span>
              <span className="text-muted">
                按顺序尝试可用渠道；本会话成功后停留在当前渠道。若当前渠道内仍有可用
                key/账号，会先在同渠道接管；仅当前渠道整体不可用时才继续往后转移。
              </span>
            </div>

            {loading ? <div className="text-muted small mb-2">加载中…</div> : null}

            <div className="row g-2 mb-3">
              <div className="col-12 col-md-8">
                <select
                  className="form-select font-monospace"
                  value={addGroup}
                  onChange={(e) => setAddGroup(e.target.value)}
                  disabled={loading || reordering}
                >
                  <option value="">选择要添加的渠道组…</option>
                  {(tokenGroupsData?.allowed_channel_groups || [])
                    .filter((g) => g.status === 1)
                    .slice()
                    .sort((a, b) => a.name.localeCompare(b.name, 'zh-CN'))
                    .map((g) => (
                      <option key={g.name} value={g.name} disabled={selectedGroupNameSet.has(g.name)}>
                        {g.name} · x{g.price_multiplier}
                      </option>
                    ))}
                </select>
              </div>
              <div className="col-12 col-md-4 d-grid">
                <button
                  type="button"
                  className="btn btn-outline-primary"
                  disabled={!addGroup || selectedGroupNameSet.has(addGroup) || loading || reordering}
                  onClick={() => {
                    addSelectedGroup(addGroup);
                    setAddGroup('');
                  }}
                >
                  添加
                </button>
              </div>
            </div>

            <DndContext
              sensors={sensors}
              collisionDetection={collisionDetection}
              onDragStart={handleDragStart}
              onDragCancel={handleDragCancel}
              onDragEnd={handleDragEnd}
            >
              <div className="fade-in-up">
                <PortalDragOverlay modifiers={[restrictToVerticalAxisModifier]} zIndex={2000}>
                  {dragOverlayChannel ? (
                    <div className="rlm-channel-dnd-overlay" style={{ width: dragOverlayWidth || undefined }}>
                      <table
                        className="table table-hover align-middle mb-0"
                        style={{ tableLayout: 'fixed', width: '100%' }}
                      >
                        {dragOverlayColWidths ? (
                          <colgroup>
                            {dragOverlayColWidths.map((w, idx) => (
                              <col key={idx} style={{ width: w }} />
                            ))}
                          </colgroup>
                        ) : null}
                        <tbody>
                          <tr className="rlm-channel-row-main rlm-channel-row-drag-preview">
                            <td className="text-center text-muted">
                              <span
                                className="d-inline-flex align-items-center justify-content-center"
                                style={{ width: 48 }}
                              >
                                <i className="ri-drag-move-2-line fs-5"></i>
                              </span>
                            </td>
                            <td className="ps-4" style={{ minWidth: 0 }}>
                              <div className="d-flex flex-column">
                                <div className="d-flex flex-wrap align-items-center gap-2">
                                  <span className="fw-bold text-dark">{dragOverlayChannel.name}</span>
                                  <span className="text-muted small">(渠道组)</span>
                                </div>
                                <div className="d-flex flex-wrap align-items-center gap-2 small text-muted mt-1">
                                  <span className="text-secondary">描述:</span>
                                  <span
                                    className="d-inline-block"
                                    style={{
                                      maxWidth: 520,
                                      whiteSpace: 'nowrap',
                                      overflow: 'hidden',
                                      textOverflow: 'ellipsis',
                                    }}
                                    title={(dragOverlayChannel.description || '').trim() || '-'}
                                  >
                                    {(dragOverlayChannel.description || '').trim() || '-'}
                                  </span>
                                </div>
                              </div>
                            </td>
                            <td>
                              <span className="badge bg-light text-dark border fw-normal">
                                {dragOverlayChannel.price_multiplier ? `x${dragOverlayChannel.price_multiplier}` : '-'}
                              </span>
                            </td>
                            <td>
                              <span
                                className={
                                  dragOverlayChannel.status === 1
                                    ? 'badge bg-success bg-opacity-10 text-success border border-success-subtle'
                                    : 'badge bg-secondary bg-opacity-10 text-secondary border'
                                }
                              >
                                {dragOverlayChannel.status === 1 ? '启用' : '禁用'}
                              </span>
                            </td>
                            <td className="text-end pe-4 text-muted small">拖动中…</td>
                          </tr>
                        </tbody>
                      </table>
                    </div>
                  ) : null}
                </PortalDragOverlay>

                <div className="card border-0 shadow-sm overflow-hidden mb-3">
                  <div className="bg-primary bg-opacity-10 py-3 px-4 d-flex justify-content-between align-items-center">
                    <div>
                      <span className="text-primary fw-bold text-uppercase small">渠道组顺序</span>
                    </div>
                    <div className="text-primary text-opacity-75 small">
                      <i className="ri-drag-move-2-line me-1"></i> 支持拖拽排序
                    </div>
                  </div>
                  <div className="table-responsive">
                    <table className="table table-hover align-middle mb-0">
                      <thead className="table-light">
                        <tr>
                          <th style={{ width: 60 }}></th>
                          <th className="ps-4">渠道组详情</th>
                          <th>倍率</th>
                          <th>状态</th>
                          <th className="text-end pe-4">操作</th>
                        </tr>
                      </thead>
                      <tbody>
                        <SortableContext items={enabledChannelIDs} strategy={verticalListSortingStrategy}>
                          {channels.map((ch) => {
                            const channelDisabled = ch.status !== 1;
                            const rowBaseClassName = [
                              'rlm-channel-row-main',
                              channelDisabled ? 'table-secondary opacity-75' : '',
                            ]
                              .filter((v) => v)
                              .join(' ');
                            const multLabel = ch.price_multiplier ? `x${ch.price_multiplier}` : '-';
                            const statusLabel = ch.status === 1 ? '启用' : '禁用';
                            const statusCls =
                              ch.status === 1
                                ? 'badge bg-success bg-opacity-10 text-success border border-success-subtle'
                                : 'badge bg-secondary bg-opacity-10 text-secondary border';

                            return (
                              <SortableRow
                                key={ch.id}
                                id={ch.id}
                                disabled={{
                                  draggable: loading || reordering || channelDisabled,
                                  droppable: loading || reordering || channelDisabled,
                                }}
                              >
                                {({ attributes, listeners, setRowRef, transform, transition, isDragging, isOver }) => {
                                  const rowClassName = [
                                    rowBaseClassName,
                                    draggingID !== null && isOver && !isDragging
                                      ? 'table-primary rlm-channel-row-drop-target'
                                      : '',
                                    isDragging ? 'rlm-channel-row-dragging' : '',
                                  ]
                                    .filter((v) => v)
                                    .join(' ');
                                  const style: CSSProperties = {
                                    transform: CSS.Transform.toString(
                                      transform ? { ...transform, x: 0, scaleX: 1, scaleY: 1 } : null
                                    ),
                                    transition: reduceMotion
                                      ? undefined
                                      : transition
                                        ? `${transition}, background-color 0.12s ease, box-shadow 0.12s ease, opacity 0.12s ease`
                                        : undefined,
                                    cursor:
                                      loading || reordering || channelDisabled
                                        ? 'not-allowed'
                                        : isDragging
                                          ? 'grabbing'
                                          : 'grab',
                                  };
                                  return (
                                    <tr
                                      ref={setRowRef}
                                      className={rowClassName || undefined}
                                      style={style}
                                      {...attributes}
                                      {...listeners}
                                      data-rlm-channel-row="main"
                                      data-rlm-channel-id={ch.id}
                                      data-rlm-channel-disabled={channelDisabled ? '1' : '0'}
                                    >
                                      <td
                                        className="text-center text-muted"
                                        title={loading || reordering ? '不可拖动' : '拖动排序'}
                                      >
                                        <span
                                          className="d-inline-flex align-items-center justify-content-center"
                                          style={{
                                            width: 48,
                                            touchAction: loading || reordering ? 'auto' : isDragging ? 'none' : 'auto',
                                          }}
                                          aria-label="拖动排序"
                                          data-rlm-drag-handle="1"
                                          onClick={(e) => e.stopPropagation()}
                                        >
                                          <i className="ri-drag-move-2-line fs-5"></i>
                                        </span>
                                      </td>
                                      <td className="ps-4" style={{ minWidth: 0 }}>
                                        <div className="d-flex flex-column">
                                          <div className="d-flex flex-wrap align-items-center gap-2">
                                            <span className="fw-bold text-dark">{ch.name}</span>
                                            <span className="text-muted small">(渠道组)</span>
                                          </div>
                                          <div className="d-flex flex-wrap align-items-center gap-2 small text-muted mt-1">
                                            <span className="text-secondary">描述:</span>
                                            <span
                                              className="d-inline-block"
                                              style={{
                                                maxWidth: 520,
                                                whiteSpace: 'nowrap',
                                                overflow: 'hidden',
                                                textOverflow: 'ellipsis',
                                              }}
                                              title={(ch.description || '').trim() || '-'}
                                            >
                                              {(ch.description || '').trim() || '-'}
                                            </span>
                                          </div>
                                        </div>
                                      </td>
                                      <td>
                                        <span className="badge bg-light text-dark border fw-normal">{multLabel}</span>
                                      </td>
                                      <td>
                                        <span className={statusCls}>{statusLabel}</span>
                                      </td>
                                      <td className="text-end pe-4 text-nowrap">
                                        <button
                                          type="button"
                                          className="btn btn-sm btn-light border text-danger"
                                          title="移除"
                                          data-rlm-dnd-ignore
                                          disabled={loading || reordering}
                                          onClick={() => removeSelectedGroup(ch.name)}
                                        >
                                          <i className="ri-close-line"></i>
                                        </button>
                                      </td>
                                    </tr>
                                  );
                                }}
                              </SortableRow>
                            );
                          })}
                          {channels.length === 0 ? (
                            <tr>
                              <td colSpan={5} className="text-center py-5 text-muted">
                                尚未选择渠道组，请先从上方下拉框添加。
                              </td>
                            </tr>
                          ) : null}
                        </SortableContext>
                      </tbody>
                    </table>
                  </div>
                </div>
              </div>
            </DndContext>

            <div className="text-muted small mb-3">
              提示：支持拖拽排序；按顺序失败转移；计费时采用最终成功渠道组的倍率。
            </div>

            <div className="d-grid d-md-flex justify-content-md-end gap-2">
              <button type="button" className="btn btn-light" data-bs-dismiss="modal" disabled={reordering}>
                关闭
              </button>
              <button
                type="button"
                className="btn btn-primary px-4"
                disabled={reordering || loading}
                onClick={() => void saveTokenGroups()}
              >
                {reordering ? '保存中…' : '保存'}
              </button>
            </div>
          </div>
        )}
      </BootstrapModal>

      <BootstrapModal
        id="modelMappingsModal"
        title={
          modelMappingsToken
            ? `模型映射：${(modelMappingsToken.name || '').trim() || `Token #${modelMappingsToken.id}`}`
            : '模型映射'
        }
        dialogClassName="modal-dialog-centered modal-lg"
        onHidden={() => {
          setModelMappingsToken(null);
          setModelMappings([]);
          setModelTargets([]);
          setModelMappingsLoaded(false);
          setModelMappingsLoading(false);
          setModelMappingsSaving(false);
          setModelMappingsErr('');
          setModelMappingsNotice('');
        }}
      >
        {!modelMappingsToken ? (
          <div className="text-muted">未选择 Token。</div>
        ) : (
          <div>
            {modelMappingsErr ? (
              <div className="alert alert-danger d-flex align-items-center" role="alert">
                <span className="me-2 material-symbols-rounded">warning</span>
                <div>{modelMappingsErr}</div>
              </div>
            ) : null}

            {modelMappingsNotice ? (
              <div className="alert alert-success d-flex align-items-center" role="alert">
                <span className="me-2 material-symbols-rounded">check_circle</span>
                <div>{modelMappingsNotice}</div>
              </div>
            ) : null}

            {modelMappingsLoading ? <div className="text-muted small mb-3">加载中…</div> : null}
            {modelMappingsLoaded && modelTargets.length === 0 ? (
              <div className="alert alert-warning d-flex align-items-center" role="alert">
                <span className="me-2 material-symbols-rounded">warning</span>
                <div>当前生效渠道组没有可用目标模型。</div>
              </div>
            ) : null}

            <div className="table-responsive mb-3">
              <table className="table table-sm align-middle mb-0" style={{ minWidth: 680 }}>
                <thead className="table-light">
                  <tr>
                    <th style={{ width: '38%' }}>输入模型</th>
                    <th>目标模型</th>
                    <th className="text-end" style={{ width: 72 }}>
                      操作
                    </th>
                  </tr>
                </thead>
                <tbody>
                  {modelMappings.map((row) => (
                    <tr key={row.rowKey}>
                      <td>
                        <input
                          type="text"
                          className="form-control form-control-sm font-monospace"
                          value={row.input_model}
                          disabled={modelMappingsLoading || modelMappingsSaving}
                          onChange={(e) => {
                            const value = e.target.value;
                            setModelMappings((prev) => {
                              return prev.map((item) =>
                                item.rowKey === row.rowKey ? { ...item, input_model: value } : item
                              );
                            });
                            setModelMappingsNotice('');
                          }}
                        />
                      </td>
                      <td>
                        <select
                          className="form-select form-select-sm font-monospace"
                          value={row.target_model}
                          disabled={modelMappingsLoading || modelMappingsSaving}
                          onChange={(e) => {
                            const value = e.target.value;
                            setModelMappings((prev) => {
                              return prev.map((item) =>
                                item.rowKey === row.rowKey ? { ...item, target_model: value } : item
                              );
                            });
                            setModelMappingsNotice('');
                          }}
                        >
                          <option value="">选择目标模型…</option>
                          {modelTargets.map((target) => (
                            <option key={target.public_id} value={target.public_id}>
                              {target.public_id}
                              {target.group_name ? ` · ${target.group_name}` : ''}
                            </option>
                          ))}
                        </select>
                      </td>
                      <td className="text-end">
                        <button
                          type="button"
                          className="btn btn-sm btn-light border text-danger"
                          title="移除"
                          disabled={modelMappingsLoading || modelMappingsSaving}
                          onClick={() => {
                            setModelMappings((prev) => prev.filter((item) => item.rowKey !== row.rowKey));
                            setModelMappingsNotice('');
                          }}
                        >
                          <i className="ri-close-line"></i>
                        </button>
                      </td>
                    </tr>
                  ))}
                  {modelMappings.length === 0 ? (
                    <tr>
                      <td colSpan={3} className="text-center py-4 text-muted">
                        暂无映射。
                      </td>
                    </tr>
                  ) : null}
                </tbody>
              </table>
            </div>

            <div className="d-grid d-md-flex justify-content-md-between gap-2">
              <button
                type="button"
                className="btn btn-light border"
                disabled={
                  !modelMappingsLoaded || modelMappingsLoading || modelMappingsSaving || modelTargets.length === 0
                }
                onClick={() => {
                  setModelMappings((prev) => [
                    ...prev,
                    makeModelMappingRow({ input_model: '', target_model: modelTargets[0]?.public_id || '' }),
                  ]);
                  setModelMappingsNotice('');
                }}
              >
                添加
              </button>
              <div className="d-grid d-md-flex justify-content-md-end gap-2">
                <button type="button" className="btn btn-light" data-bs-dismiss="modal" disabled={modelMappingsSaving}>
                  关闭
                </button>
                <button
                  type="button"
                  className="btn btn-primary"
                  disabled={!modelMappingsLoaded || modelMappingsLoading || modelMappingsSaving}
                  onClick={() => void saveModelMappings()}
                >
                  {modelMappingsSaving ? '保存中…' : '保存'}
                </button>
              </div>
            </div>
          </div>
        )}
      </BootstrapModal>

      <BootstrapModal
        id="createTokenModal"
        title="创建新 API 令牌"
        dialogClassName="modal-dialog-centered"
        headerClassName="border-bottom-0 pb-0"
        bodyClassName="pt-4"
        onHidden={() => {
          setName('');
          const tok = pendingGeneratedTokenRef.current;
          pendingGeneratedTokenRef.current = null;
          if (tok) openGeneratedTokenModal(tok);
        }}
      >
        <form
          onSubmit={async (e) => {
            e.preventDefault();
            setTokensErr('');
            try {
              const res = await createUserToken(name.trim() || undefined);
              if (!res.success) {
                throw new Error(res.message || '创建失败');
              }
              const tok = res.data?.token || '';
              pendingGeneratedTokenRef.current = tok.trim() === '' ? null : tok;
              setName('');
              closeModalById('createTokenModal');
              await refresh();
            } catch (e) {
              setTokensErr(e instanceof Error ? e.message : '创建失败');
            }
          }}
        >
          <div className="mb-3">
            <label className="form-label fw-medium text-dark">备注名称</label>
            <input
              name="name"
              type="text"
              className="form-control"
              placeholder="例如：我的项目、笔记本 CLI…"
              autoFocus
              value={name}
              onChange={(e) => setName(e.target.value)}
            />
            <div className="form-text text-muted">给令牌起个名字，方便日后管理。</div>
          </div>
          <div className="alert alert-light border mb-0 d-flex align-items-start small">
            <span className="text-primary me-2 mt-1 material-symbols-rounded">info</span>
            <div>创建成功后会弹窗展示一次；也可以在列表页查看/复制（默认隐藏）。令牌撤销后无法查看。</div>
          </div>
          <div className="modal-footer border-top-0 px-0">
            <button type="button" className="btn btn-light text-muted" data-bs-dismiss="modal">
              取消
            </button>
            <button type="submit" className="btn btn-primary px-4" disabled={tokensLoading}>
              创建
            </button>
          </div>
        </form>
      </BootstrapModal>
    </div>
  );
}
