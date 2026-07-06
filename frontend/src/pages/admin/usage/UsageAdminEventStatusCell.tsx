import type { AdminUsageEvent, UsageEventDetail } from '../../../api/admin/usage';
import {
  displayRequestedServiceTier,
  downgradedServiceTierBadgeClassName,
  downgradedServiceTierBadgeStyle,
  priorityServiceTierBadgeClassName,
  serviceTierBadgeLabel,
} from '../../usage/usageUtils';
import { badgeForState } from './usageAdminUtils';

export function UsageAdminEventStatusCell({ event, detail }: { event: AdminUsageEvent; detail?: UsageEventDetail }) {
  const pricingBreakdown = detail?.pricing_breakdown;
  const serviceTierDowngraded = pricingBreakdown?.service_tier_downgraded ?? event.service_tier_downgraded ?? false;
  const requestedServiceTier = displayRequestedServiceTier(
    pricingBreakdown?.requested_service_tier ?? event.requested_service_tier
  );
  const requestedServiceTierBadge = serviceTierBadgeLabel(requestedServiceTier);

  return (
    <td className="text-center text-nowrap">
      <span className={badgeForState(event.state_badge_class)}>{event.state_label}</span>
      {event.is_stream ? (
        <div className="badge bg-info-subtle text-info border border-info-subtle rounded-pill px-2 scale-90 mt-1">
          STREAM
        </div>
      ) : null}
      {requestedServiceTierBadge ? (
        <div
          className={serviceTierDowngraded ? downgradedServiceTierBadgeClassName : priorityServiceTierBadgeClassName}
          style={serviceTierDowngraded ? downgradedServiceTierBadgeStyle : undefined}
        >
          {requestedServiceTierBadge}
        </div>
      ) : null}
      {event.model_mismatch ? (
        <div className="badge bg-danger-subtle text-danger border border-danger-subtle rounded-pill px-2 scale-90 mt-1">
          MODEL
        </div>
      ) : null}
      {event.error ? (
        <div className="text-danger smaller mt-1" title={event.error}>
          <span className="material-symbols-rounded">error</span> 错误
        </div>
      ) : null}
    </td>
  );
}
