import type { AdminUsageEvent, UsageEventDetail } from '../../../api/admin/usage';
import { formatIntComma } from '../../../format/int';
import { providerCacheUsageRows } from '../../../modelPricingDisplay';
import { serviceTierText } from '../../usage/usageUtils';
import { formatDecimalPlain, formatUSD } from './usageAdminUtils';

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
    return <div className="text-muted small">（展开后自动加载费用计算明细）</div>;
  }

  const pricingBreakdown = detail.pricing_breakdown;

  return (
    <div className="row g-3 small">
      <DetailOverview event={event} actualServiceTier={pricingBreakdown?.service_tier || event.service_tier} />
      <PricingBreakdownSection pricingBreakdown={pricingBreakdown} />
    </div>
  );
}

function DetailOverview({ event, actualServiceTier }: { event: AdminUsageEvent; actualServiceTier?: string | null }) {
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
        <div className="text-muted smaller">Error Class</div>
        <div className="font-monospace">{event.error_class || '-'}</div>
      </div>
      <div className="col-12">
        <div className="text-muted smaller">Error Message</div>
        <div className="font-monospace text-break">{event.error_message || '-'}</div>
      </div>
      <div className="col-12 col-lg-4">
        <div className="text-muted smaller">Service Tier</div>
        <div className="font-monospace">{serviceTierText(actualServiceTier)}</div>
      </div>
    </>
  );
}

function PricingBreakdownSection({ pricingBreakdown }: { pricingBreakdown?: UsageEventDetail['pricing_breakdown'] }) {
  if (!pricingBreakdown) return null;

  const cacheRows = providerCacheUsageRows(pricingBreakdown.owned_by, pricingBreakdown);
  const cacheFormula = cacheRows.map((row) => `${row.label}×${row.label}单价`).join(' + ');
  const cacheActual = cacheRows.map((row) => (
    <span key={row.key}>
      {' + '}
      {formatIntComma(row.tokens)}×{formatUSD(row.price)}/1M
    </span>
  ));

  return (
    <div className="col-12">
      <div className="text-muted smaller">金额计算流程</div>
      <div className="font-monospace">
        <div>公式: (计费输入×输入单价 + 输出总×输出单价 + {cacheFormula}) × 生效倍率</div>
        <div className="mt-1">
          实际: ({formatIntComma(pricingBreakdown.input_tokens_billable || 0)}×
          {formatUSD(pricingBreakdown.input_usd_per_1m || '0')}/1M +{' '}
          {formatIntComma(pricingBreakdown.output_tokens_total || 0)}×
          {formatUSD(pricingBreakdown.output_usd_per_1m || '0')}/1M
          {cacheActual}) × {formatDecimalPlain(pricingBreakdown.tier_multiplier ?? 1)} ×{' '}
          {formatDecimalPlain(pricingBreakdown.channel_multiplier ?? 1)} ={' '}
          {formatUSD(pricingBreakdown.final_cost_usd || '0')}{' '}
          <span className="text-muted smaller">
            （倍率: tier×{formatDecimalPlain(pricingBreakdown.tier_multiplier ?? 1)} × channel×
            {formatDecimalPlain(pricingBreakdown.channel_multiplier ?? 1)}）
          </span>
        </div>
      </div>
    </div>
  );
}
