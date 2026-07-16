import type { AdminUsageEvent, UsageEventDetail } from '../../../api/admin/usage';
import {
  priorityServiceTierBadgeClassName,
  serviceTierBadgeLabel,
} from '../../usage/usageUtils';

export function UsageAdminEventStatusCell({ event, detail }: { event: AdminUsageEvent; detail?: UsageEventDetail }) {
  const serviceTierBadge = serviceTierBadgeLabel(
    detail?.pricing_breakdown?.service_tier ?? event.service_tier
  );

  return (
    <td className="text-center text-nowrap">
      {event.is_stream ? (
        <div className="badge bg-info-subtle text-info border border-info-subtle rounded-pill px-2 scale-90">
          STREAM
        </div>
      ) : null}
      {serviceTierBadge ? <div className={priorityServiceTierBadgeClassName}>{serviceTierBadge}</div> : null}
      {event.error ? (
        <div className="text-danger smaller mt-1" title={event.error}>
          <span className="material-symbols-rounded">error</span> 错误
        </div>
      ) : null}
      {!event.is_stream && !serviceTierBadge && !event.error ? <span className="text-muted">-</span> : null}
    </td>
  );
}
