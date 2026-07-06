import { useLayoutEffect, useMemo, useState, type CSSProperties, type RefObject } from 'react';

type PopoverPos = { left: number; top: number };

export function useAnchoredPopover(opts: {
  open: boolean;
  onClose: () => void;
  triggerRef: RefObject<HTMLElement | null>;
  panelRef: RefObject<HTMLElement | null>;
  margin?: number;
  offset?: number;
}) {
  const { open, onClose, triggerRef, panelRef } = opts;
  const margin = typeof opts.margin === 'number' ? opts.margin : 12;
  const offset = typeof opts.offset === 'number' ? opts.offset : 8;
  const [pos, setPos] = useState<PopoverPos | null>(null);

  useLayoutEffect(() => {
    if (!open) {
      const resetFrame = requestAnimationFrame(() => {
        setPos(null);
      });
      return () => {
        cancelAnimationFrame(resetFrame);
      };
    }

    const reposition = () => {
      const btn = triggerRef.current;
      const panel = panelRef.current;
      if (!btn || !panel) return;
      const btnRect = btn.getBoundingClientRect();
      const panelRect = panel.getBoundingClientRect();
      const vw = window.innerWidth || document.documentElement.clientWidth || 0;
      const vh = window.innerHeight || document.documentElement.clientHeight || 0;

      const panelW = Math.max(0, panelRect.width);
      const panelH = Math.max(0, panelRect.height);

      const maxLeft = Math.max(margin, vw - panelW - margin);
      const left = Math.min(Math.max(margin, btnRect.left), maxLeft);

      const maxTop = Math.max(margin, vh - panelH - margin);
      const top = Math.min(Math.max(margin, btnRect.bottom + offset), maxTop);

      setPos({ left, top });
    };

    const raf1 = requestAnimationFrame(() => reposition());
    const raf2 = requestAnimationFrame(() => {
      requestAnimationFrame(() => reposition());
    });

    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose();
    };
    const onPointerDown = (e: MouseEvent | PointerEvent) => {
      const target = e.target as Node | null;
      if (!target) return;
      if (panelRef.current && panelRef.current.contains(target)) return;
      if (triggerRef.current && triggerRef.current.contains(target)) return;
      onClose();
    };
    const onResize = () => reposition();
    const onScroll = () => reposition();
    window.addEventListener('keydown', onKeyDown);
    window.addEventListener('pointerdown', onPointerDown);
    window.addEventListener('resize', onResize);
    window.addEventListener('scroll', onScroll, true);
    return () => {
      window.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('pointerdown', onPointerDown);
      window.removeEventListener('resize', onResize);
      window.removeEventListener('scroll', onScroll, true);
      cancelAnimationFrame(raf1);
      cancelAnimationFrame(raf2);
    };
  }, [margin, offset, onClose, open, panelRef, triggerRef]);

  return useMemo((): CSSProperties => {
    const hidden = open && pos == null;
    return {
      position: 'fixed',
      left: pos?.left ?? margin,
      top: pos?.top ?? margin,
      visibility: hidden ? 'hidden' : 'visible',
      pointerEvents: hidden ? 'none' : undefined,
    };
  }, [margin, open, pos]);
}
