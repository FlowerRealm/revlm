import type { AdminUsageEvent, UsageEventDetail } from '../../../api/admin/usage';
import { formatIntComma } from '../../../format/int';
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
    return <div className="text-muted small">（展开后自动加载费用明细）</div>;
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

  const cacheRead = pricingBreakdown.input_tokens_cache_read || 0;
  const cacheCreate5m = pricingBreakdown.input_tokens_cache_creation_5m || 0;
  const cacheCreate1h = pricingBreakdown.input_tokens_cache_creation_1h || 0;
  const cacheCreate = pricingBreakdown.input_tokens_cache_creation || 0;

  return (
    <div className="col-12">
      <div className="text-muted smaller">费用明细</div>
      <div className="font-monospace">
        <div>
          计费输入 {formatIntComma(pricingBreakdown.input_tokens_billable || 0)} · 输出{' '}
          {formatIntComma(pricingBreakdown.output_tokens_total || 0)}
          {cacheRead > 0 ? ` · 缓存读取 ${formatIntComma(cacheRead)}` : ''}
          {cacheCreate1h > 0
            ? ` · 缓存创建·5m ${formatIntComma(cacheCreate5m)} · 缓存创建·1h ${formatIntComma(cacheCreate1h)}`
            : cacheCreate > 0
              ? ` · 缓存创建 ${formatIntComma(cacheCreate)}`
              : ''}
        </div>
        <div className="mt-1">
          合计 {formatUSD(pricingBreakdown.final_cost_usd || '0')}
          <span className="text-muted smaller">
            {' '}
            （倍率: tier×{formatDecimalPlain(pricingBreakdown.tier_multiplier ?? 1)} × channel×
            {formatDecimalPlain(pricingBreakdown.channel_multiplier ?? 1)}）
          </span>
        </div>
      </div>
    </div>
  );
}
