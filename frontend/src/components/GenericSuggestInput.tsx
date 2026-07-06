import { useEffect, useMemo, useRef, useState, type ReactNode } from 'react';

import { useAnchoredPopover } from '../hooks/useAnchoredPopover';
import { usePresence } from '../hooks/usePresence';
import { Portal } from './Portal';

export function GenericSuggestInput<T>(props: {
  id: string;
  value: string;
  disabled?: boolean;
  placeholder?: string;
  inputClassName?: string;
  minWidth?: number;
  maxWidth?: number;
  zIndex?: number;
  offset?: number;
  debounceMs?: number;
  onChange: (value: string) => void;
  onSelect: (item: T) => void;
  fetchItems: (q: string) => Promise<T[]>;
  localItems?: (q: string, fetchedItems: T[]) => T[];
  getItemKey: (item: T) => string | number;
  renderItem: (item: T) => ReactNode;
  emptyText: string;
}) {
  const {
    id,
    value,
    disabled,
    placeholder,
    inputClassName,
    minWidth,
    maxWidth,
    zIndex,
    offset,
    debounceMs,
    onChange,
    onSelect,
    fetchItems,
    localItems,
    getItemKey,
    renderItem,
    emptyText,
  } = props;

  const inputRef = useRef<HTMLInputElement | null>(null);
  const panelRef = useRef<HTMLDivElement | null>(null);
  const [focused, setFocused] = useState(false);
  const [loading, setLoading] = useState(false);
  const [items, setItems] = useState<T[]>([]);
  const [err, setErr] = useState('');
  const reqSeqRef = useRef(0);

  const q = useMemo(() => (value || '').trim(), [value]);
  const mergedItems = useMemo(() => {
    const out: T[] = [];
    const seen = new Set<string>();
    const append = (item: T) => {
      const key = String(getItemKey(item));
      if (seen.has(key)) return;
      seen.add(key);
      out.push(item);
    };
    for (const item of localItems?.(q, items) || []) append(item);
    for (const item of items) append(item);
    return out;
  }, [getItemKey, items, localItems, q]);
  const open = focused && q.length > 0 && (loading || err !== '' || mergedItems.length > 0);
  const { present, phase } = usePresence(open, 160);
  const panelStyle = useAnchoredPopover({
    open: present,
    onClose: () => setFocused(false),
    triggerRef: inputRef,
    panelRef,
    offset: typeof offset === 'number' ? offset : 4,
  });

  useEffect(() => {
    if (!focused) return;
    if (q === '') {
      setItems([]);
      setErr('');
      setLoading(false);
      return;
    }

    const seq = ++reqSeqRef.current;
    const timer = window.setTimeout(
      () => {
        void (async () => {
          setLoading(true);
          setErr('');
          try {
            const next = await fetchItems(q);
            if (reqSeqRef.current !== seq) return;
            setItems(next || []);
          } catch (e) {
            if (reqSeqRef.current !== seq) return;
            setItems([]);
            setErr(e instanceof Error ? e.message : '加载失败');
          } finally {
            if (reqSeqRef.current === seq) setLoading(false);
          }
        })();
      },
      Math.max(0, typeof debounceMs === 'number' ? debounceMs : 200)
    );

    return () => {
      window.clearTimeout(timer);
    };
  }, [fetchItems, focused, q, debounceMs]);

  const select = (item: T) => {
    onSelect(item);
    setFocused(false);
  };

  return (
    <>
      <input
        ref={inputRef}
        id={id}
        type="text"
        className={`form-control${inputClassName ? ` ${inputClassName}` : ''}`}
        placeholder={placeholder}
        value={value}
        onChange={(e) => onChange(e.target.value || '')}
        onFocus={() => setFocused(true)}
        onBlur={() => {
          window.setTimeout(() => setFocused(false), 120);
        }}
        disabled={!!disabled}
        autoComplete="off"
      />

      {present ? (
        <Portal>
          <div
            ref={panelRef}
            className={`rlm-suggest-dropdown rlm-popover ${phase === 'enter' ? 'rlm-popover-enter' : 'rlm-popover-leave'}`}
            style={{
              ...panelStyle,
              minWidth: typeof minWidth === 'number' ? minWidth : 360,
              maxWidth: typeof maxWidth === 'number' ? maxWidth : 520,
              zIndex: typeof zIndex === 'number' ? zIndex : 1080,
            }}
            onPointerDown={(e) => e.stopPropagation()}
          >
            {err ? <div className="text-danger small px-2 py-1">{err}</div> : null}
            {loading ? <div className="text-muted small px-2 py-1">加载中…</div> : null}
            {!loading && !err && mergedItems.length === 0 ? (
              <div className="text-muted small px-2 py-1">{emptyText}</div>
            ) : null}
            <div className="list-group list-group-flush">
              {mergedItems.map((it) => (
                <button
                  key={getItemKey(it)}
                  type="button"
                  className="list-group-item list-group-item-action py-2 px-2"
                  onMouseDown={(e) => {
                    e.preventDefault();
                    select(it);
                  }}
                >
                  {renderItem(it)}
                </button>
              ))}
            </div>
          </div>
        </Portal>
      ) : null}
    </>
  );
}
