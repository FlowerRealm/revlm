import type { AdminUsageEvent, UsageEventDetail } from '../../../api/admin/usage';
import { formatLatencyPairSeconds } from '../../../format/duration';
import { formatIntComma } from '../../../format/int';
import { UsageAdminEventDetail } from './UsageAdminEventDetail';
import { UsageAdminEventStatusCell } from './UsageAdminEventStatusCell';

type Props = {
  event: AdminUsageEvent;
  expanded: boolean;
  detail?: UsageEventDetail;
  detailLoading: boolean;
  onToggle: () => void;
};

export function UsageAdminEventRow({ event, expanded, detail, detailLoading, onToggle }: Props) {
  return (
    <>
      <tr role="button" onClick={onToggle}>
        <td className="ps-4 text-nowrap font-monospace">
          <i className={`ri-arrow-right-s-line text-muted me-1 align-middle ${expanded ? 'rotate-90' : ''}`}></i>
          <span className="align-middle">{event.time}</span>
        </td>
        <td className="text-nowrap">
          <div className="fw-bold small">{event.user_email}</div>
          <div className="text-muted smaller">ID: {event.user_id}</div>
        </td>
        <td className="text-nowrap">
          <div className="badge bg-light text-dark border fw-normal">{event.model}</div>
          <div className="text-muted smaller mt-1 font-monospace">{event.endpoint}</div>
        </td>
        <td className="text-center rlm-usage-cell-compact">
          {event.status_code === '200' ? (
            <span className="badge bg-success-subtle text-success border border-success-subtle rounded-pill">200</span>
          ) : (
            <span className="badge bg-danger-subtle text-danger border border-danger-subtle rounded-pill">
              {event.status_code}
            </span>
          )}
        </td>
        <td className="text-end font-monospace text-muted rlm-usage-cell-compact">
          {formatLatencyPairSeconds(event.latency_ms, event.first_token_latency_ms)}
        </td>
        <td className="text-end font-monospace rlm-usage-cell-compact">
          <TokenUsageCell event={event} />
        </td>
        <td className="text-end font-monospace text-muted rlm-usage-cell-compact">
          {formatIntComma(event.tokens_per_second)}
        </td>
        <td className="text-end font-monospace fw-bold text-dark rlm-usage-cell-compact">{event.cost_usd}</td>
        <UsageAdminEventStatusCell event={event} detail={detail} />
        <td className="text-center text-nowrap rlm-usage-cell-compact">
          {event.upstream_channel_name ? (
            <span className="badge bg-light text-dark border fw-normal">{event.upstream_channel_name}</span>
          ) : event.channel_id && event.channel_id !== '-' ? (
            <span className="badge bg-light text-dark border fw-normal">#{event.channel_id}</span>
          ) : (
            <span className="text-muted">-</span>
          )}
        </td>
        <td
          className="pe-4 font-monospace text-muted small user-select-all"
          style={{ maxWidth: 160, overflow: 'hidden', textOverflow: 'ellipsis' }}
          title={event.request_id}
        >
          {event.request_id}
        </td>
      </tr>

      {expanded ? (
        <tr className="rlm-usage-detail-row">
          <td colSpan={11} className="p-0 border-0">
            <div className="bg-light px-4 py-3 mt-1">
              <UsageAdminEventDetail event={event} detail={detail} loading={detailLoading} />
            </div>
          </td>
        </tr>
      ) : null}
    </>
  );
}

function TokenUsageCell({ event }: { event: AdminUsageEvent }) {
  return (
    <>
      <div>
        <span className="text-muted">In:</span> {formatIntComma(event.input_tokens)}
      </div>
      <div>
        <span className="text-muted">Out:</span> {formatIntComma(event.output_tokens)}
      </div>
      {event.cached_tokens !== '-' ? (
        <div className="text-muted smaller">
          <span className="material-symbols-rounded">bolt</span> {formatIntComma(event.cached_tokens)}
        </div>
      ) : null}
    </>
  );
}
