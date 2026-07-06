import { useEffect, useState } from 'react';
import { Link } from 'react-router-dom';

import { getAdminDashboard, type AdminDashboard } from '../../api/admin/dashboard';
import { getAdminUsageTimeSeries, type AdminUsageTimeSeriesPoint } from '../../api/admin/usage';
import { useAuth } from '../../auth/AuthContext';
import { SegmentedFrame } from '../../components/SegmentedFrame';
import { formatIntComma } from '../../format/int';
import { fillDailyBuckets } from '../../utils/timeSeries';
import { UsageAdminTimeSeriesCard } from './usage/UsageAdminTimeSeriesCard';
import {
  type UsageAdminDetailField,
  type UsageAdminDetailGranularity,
  usageAdminFieldOptions,
  usageAdminGranularityOptions,
} from './usage/usageAdminUtils';

export function AdminDashboardPage() {
  const { user } = useAuth();
  const [data, setData] = useState<AdminDashboard | null>(null);
  const [err, setErr] = useState('');

  const [detailSeries, setDetailSeries] = useState<AdminUsageTimeSeriesPoint[]>([]);
  const [detailSeriesStart, setDetailSeriesStart] = useState('');
  const [detailSeriesEnd, setDetailSeriesEnd] = useState('');
  const [detailSeriesLoading, setDetailSeriesLoading] = useState(false);
  const [detailSeriesErr, setDetailSeriesErr] = useState('');
  const [detailField, setDetailField] = useState<UsageAdminDetailField>('requests');
  const [detailGranularity, setDetailGranularity] = useState<UsageAdminDetailGranularity>('hour');

  useEffect(() => {
    let mounted = true;
    (async () => {
      setErr('');
      try {
        const res = await getAdminDashboard();
        if (!res.success) throw new Error(res.message || '加载失败');
        if (mounted) setData(res.data || null);
      } catch (e) {
        if (!mounted) return;
        setErr(e instanceof Error ? e.message : '加载失败');
        setData(null);
      }
    })();
    return () => {
      mounted = false;
    };
  }, []);

  useEffect(() => {
    let active = true;
    void (async () => {
      setDetailSeriesErr('');
      setDetailSeriesLoading(true);
      try {
        const res = await getAdminUsageTimeSeries({ granularity: detailGranularity });
        if (!res.success) throw new Error(res.message || '加载时间序列失败');
        if (!active) return;
        const start = res.data?.start || '';
        const end = res.data?.end || '';
        const points = res.data?.points || [];
        setDetailSeriesStart(start);
        setDetailSeriesEnd(end);
        setDetailSeries(
          detailGranularity === 'day'
            ? fillDailyBuckets(points, start, end, (bucket) => ({
                bucket,
                requests: 0,
                tokens: 0,
                committed_usd: 0,
                cache_ratio: 0,
                avg_first_token_latency: 0,
                tokens_per_second: 0,
              }))
            : points
        );
      } catch (e) {
        if (!active) return;
        setDetailSeries([]);
        setDetailSeriesStart('');
        setDetailSeriesEnd('');
        setDetailSeriesErr(e instanceof Error ? e.message : '加载时间序列失败');
      } finally {
        if (active) setDetailSeriesLoading(false);
      }
    })();
    return () => {
      active = false;
    };
  }, [detailGranularity]);

  if (err) {
    return (
      <div className="alert alert-danger mb-4" role="alert">
        <i className="ri-alert-line me-2"></i>
        {err}
      </div>
    );
  }

  if (!data) {
    return (
      <div className="card border-0 shadow-sm">
        <div className="card-body text-muted small d-flex align-items-center">
          <span className="spinner-border spinner-border-sm me-2" role="status" aria-hidden="true"></span>
          正在加载…
        </div>
      </div>
    );
  }

  const tz = data.admin_time_zone || 'Asia/Shanghai';
  const stats = data.stats;

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <div className="d-flex align-items-center justify-content-between">
          <h2 className="h4 fw-bold mb-0 text-dark">仪表盘</h2>
          <span className="badge bg-white text-secondary border shadow-sm">{tz} 时间</span>
        </div>

        <div className="row g-4 mb-0">
          <MetricCard icon="ri-group-line" label="总用户数" value={formatIntComma(stats.users_count)} tone="primary" />
          <MetricCard
            icon="ri-git-merge-line"
            label="上游渠道"
            value={formatIntComma(stats.channels_count)}
            tone="success"
          />
          <MetricCard
            icon="ri-server-line"
            label="上游节点"
            value={formatIntComma(stats.endpoints_count)}
            tone="info"
          />
        </div>

        <div className="card border-0 shadow-sm mb-0 overflow-hidden">
          <div className="card-header bg-white border-bottom py-3 px-4 d-flex justify-content-between align-items-center">
            <div className="d-flex align-items-center">
              <div className="rounded-circle bg-primary p-1 me-2"></div>
              <span className="fw-bold text-dark text-uppercase small">今日概览</span>
              <span className="text-muted smaller ms-2">{tz} 时间</span>
            </div>
            <div className="text-muted smaller">
              <i className="ri-time-line me-1 text-primary"></i> 当前快照
            </div>
          </div>
          <div className="card-body p-4">
            <div className="row text-center">
              <div className="col-md-4 border-end">
                <h6 className="text-muted mb-2 small fw-semibold text-uppercase">总请求数</h6>
                <h2 className="fw-bold text-dark">{formatIntComma(stats.requests_today)}</h2>
              </div>
              <div className="col-md-4 border-end">
                <h6 className="text-muted mb-2 small fw-semibold text-uppercase">Token 消耗</h6>
                <h2 className="fw-bold text-dark">{formatIntComma(stats.tokens_today)}</h2>
                <div className="small text-muted font-monospace mt-1">
                  <span className="me-2">
                    <i className="ri-arrow-up-line text-success"></i> {formatIntComma(stats.input_tokens_today)}
                  </span>
                  <span>
                    <i className="ri-arrow-down-line text-primary"></i> {formatIntComma(stats.output_tokens_today)}
                  </span>
                </div>
              </div>
              <div className="col-md-4">
                <h6 className="text-muted mb-2 small fw-semibold text-uppercase">预估消费</h6>
                <h2 className="fw-bold text-primary">{stats.cost_today}</h2>
              </div>
            </div>
          </div>
        </div>

        <UsageAdminTimeSeriesCard
          detailSeries={detailSeries}
          detailSeriesStart={detailSeriesStart}
          detailSeriesEnd={detailSeriesEnd}
          detailSeriesErr={detailSeriesErr}
          detailSeriesLoading={detailSeriesLoading}
          detailField={detailField}
          detailGranularity={detailGranularity}
          fieldOptions={usageAdminFieldOptions}
          granularityOptions={usageAdminGranularityOptions}
          onFieldChange={setDetailField}
          onGranularityChange={setDetailGranularity}
        />

        <div className="row g-4 mb-0">
          <div className="col-md-6">
            <div className="card h-100 border-0 shadow-sm mb-0">
              <div className="card-body">
                <h5 className="card-title fw-bold mb-3 text-dark h6">快捷操作</h5>
                <div className="d-grid gap-2">
                  <Link
                    to="/admin/channels"
                    className="btn btn-outline-primary text-start border-light shadow-sm text-dark hover-white"
                  >
                    <i className="ri-git-merge-line me-2 text-primary"></i> 管理上游渠道
                  </Link>
                  <Link
                    to="/admin/users"
                    className="btn btn-outline-primary text-start border-light shadow-sm text-dark hover-white"
                  >
                    <i className="ri-user-settings-line me-2 text-primary"></i> 管理用户与权限
                  </Link>
                </div>
              </div>
            </div>
          </div>

          <div className="col-md-6">
            <div className="card h-100 border-0 shadow-sm mb-0">
              <div className="card-body">
                <h5 className="card-title fw-bold mb-3 text-dark h6">系统信息</h5>
                <ul className="list-unstyled mb-0">
                  <li className="mb-3 d-flex align-items-center">
                    <span className="text-muted small me-2">当前用户:</span>
                    <strong className="text-dark">{user?.email || '-'}</strong>
                  </li>
                  <li className="mb-3 d-flex align-items-center">
                    <span className="text-muted small me-2">角色权限:</span>
                    <span className="badge bg-primary bg-opacity-10 text-primary px-3 py-2 rounded-pill">
                      {user?.role || '-'}
                    </span>
                  </li>
                </ul>
              </div>
            </div>
          </div>
        </div>
      </SegmentedFrame>
    </div>
  );
}

function MetricCard({ icon, label, value, tone }: { icon: string; label: string; value: string; tone: string }) {
  return (
    <div className="col-md-4">
      <div
        className="card h-100 border-0 shadow-sm metric-card mb-0"
        style={{ borderTop: `3px solid var(--bs-${tone})` }}
      >
        <div className="card-body d-flex align-items-center">
          <div className={`bg-${tone} bg-opacity-10 p-3 rounded-circle me-3`}>
            <i className={`${icon} fs-4 text-${tone}`}></i>
          </div>
          <div>
            <h6 className="text-muted text-uppercase mb-1 small fw-semibold">{label}</h6>
            <h3 className="mb-0 fw-bold text-dark">{value}</h3>
          </div>
        </div>
      </div>
    </div>
  );
}
