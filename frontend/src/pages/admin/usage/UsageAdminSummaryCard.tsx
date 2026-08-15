import type { AdminUsageWindow } from '../../../api/admin/usage';
import { formatSecondsFromMilliseconds } from '../../../format/duration';
import { formatIntComma } from '../../../format/int';

export function UsageAdminSummaryCard({ windowStats }: { windowStats: AdminUsageWindow }) {
  return (
    <div className="card border-0 overflow-hidden">
      <div className="bg-primary bg-opacity-10 py-3 px-4 d-flex justify-content-between align-items-center">
        <div>
          <span className="text-primary fw-bold text-uppercase small">{windowStats.window}</span>
          <span className="text-primary text-opacity-75 smaller ms-2">
            统计区间: {windowStats.since} ~ {windowStats.until}
          </span>
        </div>
        <div className="text-primary text-opacity-75 smaller">
          <i className="ri-shield-check-line me-1"></i> 实时统计
        </div>
      </div>
      <div className="card-body p-4">
        <div className="row g-4">
          <div className="col-lg-4 border-end">
            <div className="mb-4">
              <div className="text-muted smaller mb-1">消耗（USD）</div>
              <h1 className="display-6 fw-bold mb-0 text-dark">{windowStats.usd}</h1>
            </div>
          </div>

          <div className="col-lg-8 ps-lg-4">
            <div className="row g-3">
              <MetricCard
                label="全局请求数"
                value={formatIntComma(windowStats.requests)}
                detail={`${formatIntComma(windowStats.rpm)} RPM`}
                detailClassName="text-primary"
              />
              <MetricCard
                label="平均首字延迟"
                value={formatSecondsFromMilliseconds(windowStats.avg_first_token_latency)}
                detail="基于有效首字样本"
              />
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}

function MetricCard({
  label,
  value,
  detail,
  detailClassName = 'text-muted',
}: {
  label: string;
  value: string;
  detail: string;
  detailClassName?: string;
}) {
  return (
    <div className="col-sm-6 col-md-3">
      <div className="metric-card p-3 rounded-3 border">
        <div className="text-muted smaller mb-1">{label}</div>
        <div className="h4 fw-bold mb-1">{value}</div>
        <div className={`${detailClassName} smaller fw-medium`}>{detail}</div>
      </div>
    </div>
  );
}
