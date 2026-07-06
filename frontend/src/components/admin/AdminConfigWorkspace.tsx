import type { ReactNode } from 'react';

export type AdminConfigSectionItem = {
  id: string;
  label: string;
  hint?: string;
};

export function AdminConfigWorkspace({
  sidebar,
  detail,
  className,
}: {
  sidebar: ReactNode;
  detail: ReactNode;
  className?: string;
}) {
  return (
    <div className={['rlm-admin-config-workspace', className].filter(Boolean).join(' ')}>
      {sidebar}
      {detail}
    </div>
  );
}

export function AdminConfigSidebar({
  title,
  description,
  toolbar,
  children,
}: {
  title: string;
  description?: ReactNode;
  toolbar?: ReactNode;
  children: ReactNode;
}) {
  return (
    <aside className="rlm-admin-config-sidebar">
      <div className="rlm-admin-config-sidebar-card">
        <div className="rlm-admin-config-sidebar-header">
          <div>
            <h5 className="mb-1 fw-semibold">{title}</h5>
            {description ? <div className="text-muted small">{description}</div> : null}
          </div>
          {toolbar ? <div className="rlm-admin-config-sidebar-toolbar">{toolbar}</div> : null}
        </div>
        <div className="rlm-admin-config-sidebar-body">{children}</div>
      </div>
    </aside>
  );
}

export function AdminConfigDetail({
  header,
  sectionNav,
  children,
}: {
  header?: ReactNode;
  sectionNav?: ReactNode;
  children: ReactNode;
}) {
  return (
    <section className="rlm-admin-config-detail">
      <div className="rlm-admin-config-detail-card">
        {header ? <div className="rlm-admin-config-detail-header">{header}</div> : null}
        {sectionNav ? <div className="rlm-admin-config-detail-nav">{sectionNav}</div> : null}
        <div className="rlm-admin-config-detail-body">{children}</div>
      </div>
    </section>
  );
}

export function AdminConfigSectionNav({ items, className }: { items: AdminConfigSectionItem[]; className?: string }) {
  return (
    <div className={['rlm-admin-config-section-nav', className].filter(Boolean).join(' ')}>
      {items.map((item) => (
        <a key={item.id} className="rlm-admin-config-section-link" href={`#${item.id}`}>
          <span>{item.label}</span>
          {item.hint ? <span className="text-muted smaller">{item.hint}</span> : null}
        </a>
      ))}
    </div>
  );
}

export function AdminConfigSection({
  id,
  title,
  description,
  actions,
  children,
  className,
}: {
  id: string;
  title: string;
  description?: ReactNode;
  actions?: ReactNode;
  children: ReactNode;
  className?: string;
}) {
  return (
    <section id={id} className={['rlm-admin-config-section', className].filter(Boolean).join(' ')}>
      <div className="rlm-admin-config-section-head">
        <div>
          <h5 className="mb-1 fw-semibold">{title}</h5>
          {description ? <div className="text-muted small">{description}</div> : null}
        </div>
        {actions ? <div className="rlm-admin-config-section-actions">{actions}</div> : null}
      </div>
      <div className="rlm-admin-config-section-content">{children}</div>
    </section>
  );
}

export function AdminConfigList({ children }: { children: ReactNode }) {
  return <div className="rlm-admin-config-list">{children}</div>;
}

export function AdminConfigListItem({
  active,
  title,
  meta,
  hint,
  badge,
  onClick,
}: {
  active?: boolean;
  title: ReactNode;
  meta?: ReactNode;
  hint?: ReactNode;
  badge?: ReactNode;
  onClick?: () => void;
}) {
  return (
    <button type="button" className={`rlm-admin-config-list-item${active ? ' active' : ''}`} onClick={onClick}>
      <div className="rlm-admin-config-list-item-top">
        <div className="rlm-admin-config-list-item-title">{title}</div>
        {badge ? <div className="rlm-admin-config-list-item-badge">{badge}</div> : null}
      </div>
      {meta ? <div className="rlm-admin-config-list-item-meta">{meta}</div> : null}
      {hint ? <div className="rlm-admin-config-list-item-hint">{hint}</div> : null}
    </button>
  );
}
