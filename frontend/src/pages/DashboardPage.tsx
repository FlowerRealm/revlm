import { useEffect, useState } from 'react';
import { Link } from 'react-router-dom';

import { getDashboard, type DashboardData } from '../api/dashboard';
import { getUsageTimeSeries, type UsageTimeSeriesPoint } from '../api/usage';
import { SegmentedFrame } from '../components/SegmentedFrame';
import { formatIntComma } from '../format/int';
import { fillDailyBuckets } from '../utils/timeSeries';
import { UsageTimeSeriesCard } from './usage/UsageTimeSeriesCard';

type DetailField =
  | 'committed_usd'
  | 'requests'
  | 'tokens'
  | 'cache_ratio'
  | 'avg_first_token_latency'
  | 'tokens_per_second';
type DetailGranularity = 'hour' | 'day';

export function DashboardPage() {
  const [data, setData] = useState<DashboardData | null>(null);
  const [err, setErr] = useState('');

  const [detailSeries, setDetailSeries] = useState<UsageTimeSeriesPoint[]>([]);
  const [detailSeriesStart, setDetailSeriesStart] = useState('');
  const [detailSeriesEnd, setDetailSeriesEnd] = useState('');
  const [detailSeriesLoading, setDetailSeriesLoading] = useState(false);
  const [detailSeriesErr, setDetailSeriesErr] = useState('');
  const [detailField, setDetailField] = useState<DetailField>('requests');
  const [detailGranularity, setDetailGranularity] = useState<DetailGranularity>('hour');
  const fieldOptions: Array<{
    value: DetailField;
    label: string;
  }> = [
    { value: 'requests', label: '请求数' },
    { value: 'tokens', label: 'Token' },
    { value: 'committed_usd', label: '消耗 (USD)' },
    { value: 'cache_ratio', label: '缓存率 (%)' },
    { value: 'avg_first_token_latency', label: '首字延迟 (s)' },
    { value: 'tokens_per_second', label: 'Tokens/s' },
  ];
  const granularityOptions: Array<{ value: DetailGranularity; label: string }> = [
    { value: 'hour', label: '按小时' },
    { value: 'day', label: '按天' },
  ];

  useEffect(() => {
    let mounted = true;
    (async () => {
      setErr('');
      try {
        const res = await getDashboard();
        if (!res.success) {
          throw new Error(res.message || '加载失败');
        }
        if (mounted) {
          setData(res.data || null);
        }
      } catch (e) {
        if (mounted) {
          setErr(e instanceof Error ? e.message : '加载失败');
          setData(null);
        }
      }
    })();
    return () => {
      mounted = false;
    };
  }, []);

  useEffect(() => {
    let active = true;
    if (detailGranularity === 'hour') {
      setDetailSeriesErr('');
      setDetailSeriesLoading(!data && !err);
      setDetailSeriesStart(data?.today_since || '');
      setDetailSeriesEnd(data?.today_until || '');
      setDetailSeries(data?.charts.time_series_stats || []);
      return () => {
        active = false;
      };
    }
    void (async () => {
      setDetailSeriesErr('');
      setDetailSeriesLoading(true);
      try {
        const res = await getUsageTimeSeries(undefined, undefined, detailGranularity);
        if (!res.success) throw new Error(res.message || '时间序列加载失败');
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
        setDetailSeriesErr(e instanceof Error ? e.message : '时间序列加载失败');
      } finally {
        if (active) setDetailSeriesLoading(false);
      }
    })();
    return () => {
      active = false;
    };
  }, [data, detailGranularity, err]);

  const todayUsageUSD = data?.today_usage_usd || '-';
  const todayRequests = data ? formatIntComma(data.today_requests) : '-';
  const todayRPM = data ? formatIntComma(data.today_rpm) : '-';
  const todayTokens = data ? formatIntComma(data.today_tokens) : '-';
  const todayTPM = data ? formatIntComma(data.today_tpm) : '-';

  return (
    <div className="fade-in-up">
      {err ? (
        <div className="alert alert-danger d-flex align-items-center" role="alert">
          <span className="me-2 material-symbols-rounded">warning</span>
          <div>{err}</div>
        </div>
      ) : null}

      <SegmentedFrame>
        <div className="row g-4">
          <div className="col-12">
            <div className="row g-4">
              <div className="col-md-6 col-xl-3">
                <div className="card h-100 mb-0">
                  <div className="card-body">
                    <div className="d-flex align-items-center mb-3">
                      <div className="bg-primary bg-opacity-10 text-primary rounded-pill p-2 me-3">
                        <span className="fs-4 px-1 material-symbols-rounded">attach_money</span>
                      </div>
                      <h6 className="card-title mb-0 fw-bold">今日费用</h6>
                    </div>
                    <div className="mb-0">
                      <h3 className="fw-bold mb-1">{todayUsageUSD}</h3>
                      <p className="text-muted small mb-0">预估消耗 (USD)</p>
                    </div>
                  </div>
                </div>
              </div>

              <div className="col-md-6 col-xl-3">
                <div className="card h-100 mb-0">
                  <div className="card-body">
                    <div className="d-flex align-items-center mb-3">
                      <div className="bg-info bg-opacity-10 text-info rounded-pill p-2 me-3">
                        <span className="fs-4 px-1 material-symbols-rounded">chat</span>
                      </div>
                      <h6 className="card-title mb-0 fw-bold">今日请求</h6>
                    </div>
                    <div className="mb-0">
                      <h3 className="fw-bold mb-1">{todayRequests}</h3>
                      <div className="text-muted small">
                        <span className="badge bg-light text-secondary border fw-normal">RPM: {todayRPM}</span>
                        <span className="ms-1">次/分钟</span>
                      </div>
                    </div>
                  </div>
                </div>
              </div>

              <div className="col-md-6 col-xl-3">
                <div className="card h-100 mb-0">
                  <div className="card-body">
                    <div className="d-flex align-items-center mb-3">
                      <div className="bg-success bg-opacity-10 text-success rounded-pill p-2 me-3">
                        <span className="fs-4 px-1 material-symbols-rounded">memory</span>
                      </div>
                      <h6 className="card-title mb-0 fw-bold">今日 Token</h6>
                    </div>
                    <div className="mb-0">
                      <h3 className="fw-bold mb-1">{todayTokens}</h3>
                      <div className="text-muted small">
                        <span className="badge bg-light text-secondary border fw-normal">TPM: {todayTPM}</span>
                        <span className="ms-1">Tokens/分钟</span>
                      </div>
                    </div>
                  </div>
                </div>
              </div>

              <div className="col-md-6 col-xl-3">
                <div className="card h-100 border-dashed mb-0">
                  <div className="card-body d-flex flex-column align-items-center justify-content-center text-center py-4">
                    <div className="bg-light text-muted rounded-circle p-2 mb-2">
                      <span className="fs-4 material-symbols-rounded">account_balance_wallet</span>
                    </div>
                    <h6 className="fw-bold small mb-1">按量计费</h6>
                    <p className="text-muted small mb-2">模型调用直接从余额扣费。</p>
                    <Link to="/topup" className="btn btn-outline-primary btn-sm px-3 py-1 smaller">
                      余额充值
                    </Link>
                  </div>
                </div>
              </div>
            </div>
          </div>
        </div>

        <UsageTimeSeriesCard
          rangeSinceText={detailSeriesStart || '-'}
          rangeUntilText={detailSeriesEnd || '-'}
          detailSeries={detailSeries}
          detailSeriesErr={detailSeriesErr}
          detailSeriesLoading={detailSeriesLoading}
          detailField={detailField}
          setDetailField={setDetailField}
          detailGranularity={detailGranularity}
          setDetailGranularity={setDetailGranularity}
          fieldOptions={fieldOptions}
          granularityOptions={granularityOptions}
        />
      </SegmentedFrame>
    </div>
  );
}
