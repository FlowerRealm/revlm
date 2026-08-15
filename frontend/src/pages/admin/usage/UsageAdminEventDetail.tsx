import type { AdminUsageEvent, UsageEventDetail } from '../../../api/admin/usage';
import { formatDecimalPlain } from './usageAdminUtils';

export function UsageAdminEventDetail({
  event,
  detail,
  loading,
}: {
  event: AdminUsageEvent;
  detail?: UsageEventDetail;
  loading: boolean;
}) {
  if (loading) {
    return <div className="text-muted small">加载详情中…</div>;
  }

  if (!detail) {
    return <div className="text-muted small">（展开后自动加载请求详情）</div>;
  }

  return (
    <div className="row g-3 small">
      <DetailOverview event={event} />
      <UsageDetailsSection detail={detail} event={event} />
    </div>
  );
}

function DetailOverview({ event }: { event: AdminUsageEvent }) {
  return (
    <>
      <div className="col-12 col-lg-4">
        <div className="text-muted smaller">Event ID</div>
        <div className="font-monospace">{event.id}</div>
      </div>
      <div className="col-12 col-lg-4">
        <div className="text-muted smaller">Request ID</div>
        <div className="font-monospace user-select-all">{event.request_id || '-'}</div>
      </div>
      <div className="col-12 col-lg-4">
        <div className="text-muted smaller">Response ID</div>
        <div className="font-monospace user-select-all">{event.response_id || '-'}</div>
      </div>
      <div className="col-12 col-lg-4">
        <div className="text-muted smaller">渠道组倍率</div>
        <div className="font-monospace">{formatDecimalPlain(event.channel_group_multiplier ?? 1)}</div>
      </div>
      <div className="col-12">
        <div className="text-muted smaller">Error Message</div>
        <div className="font-monospace text-break">{event.error_message || '-'}</div>
      </div>
    </>
  );
}

// The raw usage payload, shown as it was recorded. Its fields belong to the
// plugin that served the request, so the console renders the JSON instead of
// naming keys it cannot know.
function UsageDetailsSection({ detail, event }: { detail: UsageEventDetail; event: AdminUsageEvent }) {
  const usageDetails = detail.usage_details ?? event.usage_details;
  const text = usageDetails && Object.keys(usageDetails).length > 0 ? JSON.stringify(usageDetails, null, 2) : '';

  return (
    <div className="col-12">
      <div className="text-muted smaller">usage_details</div>
      {text ? (
        <pre className="font-monospace small mb-0 mt-1 p-2 bg-white border rounded overflow-auto">{text}</pre>
      ) : (
        <div className="font-monospace">-</div>
      )}
    </div>
  );
}
