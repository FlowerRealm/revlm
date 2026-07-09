import type { AdminUsageEvent, UsageEventDetail } from '../../../api/admin/usage';
import {
  priorityServiceTierBadgeClassName,
  serviceTierBadgeLabel,
} from '../../usage/usageUtils';
import { badgeForState } from './usageAdminUtils';

export function UsageAdminEventStatusCell({ event, detail }: { event: AdminUsageEvent; detail?: UsageEventDetail }) {
  const serviceTierBadge = serviceTierBadgeLabel(
    detail?.pricing_breakdown?.service_tier ?? event.service_tier
  );

  return (
    <td className="text-center text-nowrap">
      <span className={badgeForState(event.state_badge_class)}>{event.state_label}</span>
      {event.is_stream ? (
        <div className="badge bg-info-subtle text-info border border-info-subtle rounded-pill px-2 scale-90 mt-1">
          STREAM
        </div>
      ) : null}
      {serviceTierBadge ? <div className={priorityServiceTierBadgeClassName}>{serviceTierBadge}</div> : null}
      {event.error ? (
        <div className="text-danger smaller mt-1" title={event.error}>
          <span className="material-symbols-rounded">error</span> 错误
        </div>
      ) : null}
    </td>
  );
}
