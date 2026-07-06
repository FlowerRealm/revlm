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
              <div className="text-muted smaller mb-1">总营收流水（USD）</div>
              <h1 className="display-6 fw-bold mb-0 text-dark">{windowStats.committed_usd}</h1>
            </div>
            <div className="row g-0 py-3 bg-light rounded-3 px-3">
              <div className="col-12">
                <div className="text-muted smaller">已结算</div>
                <div className="fw-bold h5 mb-0 text-success">{windowStats.committed_usd}</div>
              </div>
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
                label="Token 吞吐"
                value={formatIntComma(windowStats.tokens)}
                detail={`${formatIntComma(windowStats.tpm)} TPM`}
                detailClassName="text-primary"
              />
              <MetricCard label="缓存率" value={windowStats.cache_ratio} detail="输入 + 输出" />
              <MetricCard label="缓存 Token" value={formatIntComma(windowStats.cached_tokens)} detail="输入 + 输出" />
              <MetricCard
                label="平均首字延迟"
                value={formatSecondsFromMilliseconds(windowStats.avg_first_token_latency)}
                detail="基于有效首字样本"
              />
              <MetricCard
                label="平均 Tokens/s"
                value={windowStats.tokens_per_second || '-'}
                detail="输出 Token 解码速率"
              />

              <div className="col-12 mt-3">
                <div className="bg-light p-3 rounded-3">
                  <div className="row text-center small">
                    <div className="col-6 border-end">
                      <div className="text-muted smaller">输入总计</div>
                      <div className="fw-medium">{formatIntComma(windowStats.input_tokens)}</div>
                    </div>
                    <div className="col-6">
                      <div className="text-muted smaller">输出总计</div>
                      <div className="fw-medium">{formatIntComma(windowStats.output_tokens)}</div>
                    </div>
                  </div>
                </div>
              </div>
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
