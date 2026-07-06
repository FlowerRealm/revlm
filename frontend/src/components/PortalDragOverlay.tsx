import { DragOverlay, type DragOverlayProps } from '@dnd-kit/core';
import { createPortal } from 'react-dom';

export function PortalDragOverlay({ container, ...props }: DragOverlayProps & { container?: Element | null }) {
  const target = container ?? (typeof document !== 'undefined' ? document.body : null);
  if (!target) return <DragOverlay {...props} />;
  return createPortal(<DragOverlay {...props} />, target);
}
