import { useEffect, useId, useRef, type ReactNode } from 'react';
import { createPortal } from 'react-dom';

type BootstrapModalProps = {
  id: string;
  title: ReactNode;
  children: ReactNode;

  dialogClassName?: string;
  contentClassName?: string;
  headerClassName?: string;
  bodyClassName?: string;
  footerClassName?: string;

  footer?: ReactNode;
  closeLabel?: string;
  onHidden?: () => void;
};

export function BootstrapModal({
  id,
  title,
  children,
  dialogClassName,
  contentClassName,
  headerClassName,
  bodyClassName,
  footerClassName,
  footer,
  closeLabel,
  onHidden,
}: BootstrapModalProps) {
  const titleId = useId();
  const modalRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    const el = modalRef.current;
    if (!el || !onHidden) return;
    const handler = () => {
      onHidden();
    };
    el.addEventListener('hidden.bs.modal', handler);
    return () => {
      el.removeEventListener('hidden.bs.modal', handler);
    };
  }, [onHidden]);

  const modal = (
    <div className="modal fade" id={id} tabIndex={-1} aria-hidden="true" aria-labelledby={titleId} ref={modalRef}>
      <div className={`modal-dialog ${dialogClassName || ''}`.trim()}>
        <div className={`modal-content ${contentClassName || 'border-0 shadow'}`.trim()}>
          <div className={`modal-header ${headerClassName || ''}`.trim()}>
            <h5 className="modal-title fw-bold" id={titleId}>
              {title}
            </h5>
            <button
              type="button"
              className="btn-close"
              data-bs-dismiss="modal"
              aria-label={closeLabel || '关闭'}
            ></button>
          </div>
          <div className={`modal-body ${bodyClassName || ''}`.trim()}>{children}</div>
          {footer ? <div className={`modal-footer ${footerClassName || 'border-top-0'}`.trim()}>{footer}</div> : null}
        </div>
      </div>
    </div>
  );

  // 对齐 SSR：将 modal 挂到 body，避免 backdrop/层级异常导致“页面灰幕但弹窗不可见”
  if (typeof document !== 'undefined') return createPortal(modal, document.body);
  return modal;
}
