import type { AdminUsageEvent, UsageEventDetail } from '../../../api/admin/usage';
import { UsageAdminEventRow } from './UsageAdminEventRow';

type Props = {
  events: AdminUsageEvent[];
  expandedID: number | null;
  detailByEventID: Record<number, UsageEventDetail>;
  detailLoadingID: number | null;
  canPrev: boolean;
  canNext: boolean;
  loading: boolean;
  onToggleEvent: (eventID: number) => void;
  onPrevPage: () => void;
  onNextPage: () => void;
};

export function UsageAdminEventsCard({
  events,
  expandedID,
  detailByEventID,
  detailLoadingID,
  canPrev,
  canNext,
  loading,
  onToggleEvent,
  onPrevPage,
  onNextPage,
}: Props) {
  return (
    <div className="card border-0 p-0 overflow-hidden">
      <div className="card-header bg-white py-3 border-bottom-0 px-4 d-flex justify-content-between align-items-center">
        <h5 className="mb-0 fw-bold">
          <i className="ri-list-check-2 me-2"></i>请求明细
        </h5>
        <div className="d-flex gap-2">
          <button
            type="button"
            className="btn btn-sm btn-outline-secondary"
            disabled={!canPrev || loading}
            onClick={onPrevPage}
          >
            上一页
          </button>
          <button
            type="button"
            className="btn btn-sm btn-outline-secondary"
            disabled={!canNext || loading}
            onClick={onNextPage}
          >
            下一页
          </button>
        </div>
      </div>
      <div className="card-body p-0 border-top">
        <div className="table-responsive rlm-table-responsive-no-x">
          <table className="table table-hover align-middle mb-0 border-0 rlm-table-fit">
            <colgroup>
              <col />
              <col />
              <col />
              <col className="rlm-usage-col-status" />
              <col className="rlm-usage-col-latency" />
              <col className="rlm-usage-col-tokens" />
              <col className="rlm-usage-col-tps" />
              <col className="rlm-usage-col-cost" />
              <col />
              <col className="rlm-usage-col-channel" />
              <col className="rlm-usage-col-request" />
            </colgroup>
            <thead className="table-light text-muted smaller uppercase">
              <tr>
                <th className="ps-4 border-0">时间</th>
                <th className="border-0">用户</th>
                <th className="border-0">接口 / 模型</th>
                <th className="text-center border-0 rlm-usage-cell-compact">状态码</th>
                <th className="text-end border-0 rlm-usage-cell-compact">耗时/首字</th>
                <th className="text-end border-0 rlm-usage-cell-compact">Tokens</th>
                <th className="text-end border-0 rlm-usage-cell-compact">Tokens/s</th>
                <th className="text-end border-0 rlm-usage-cell-compact">费用</th>
                <th className="text-center border-0">状态</th>
                <th className="text-center border-0 rlm-usage-cell-compact">渠道</th>
                <th className="pe-4 border-0">Request ID</th>
              </tr>
            </thead>
            <tbody className="small">
              {events.map((event) => (
                <UsageAdminEventRow
                  key={event.id}
                  event={event}
                  expanded={expandedID === event.id}
                  detail={detailByEventID[event.id]}
                  detailLoading={detailLoadingID === event.id}
                  onToggle={() => onToggleEvent(event.id)}
                />
              ))}
              {events.length === 0 ? (
                <tr>
                  <td colSpan={11} className="text-center py-5 text-muted small">
                    暂无请求记录
                  </td>
                </tr>
              ) : null}
            </tbody>
          </table>
        </div>
      </div>
    </div>
  );
}
