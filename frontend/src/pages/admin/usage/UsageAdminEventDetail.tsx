import type { AdminUsageEvent, UsageEventDetail } from '../../../api/admin/usage';
import { formatIntComma } from '../../../format/int';
import { providerCacheUsageRows } from '../../../modelPricingDisplay';
import { displayRequestedServiceTier, serviceTierDowngradeText, serviceTierText } from '../../usage/usageUtils';
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
  const modelCheck = detail.model_check;
  const serviceTierDowngraded = pricingBreakdown?.service_tier_downgraded ?? event.service_tier_downgraded ?? false;
  const serviceTierDowngradeReason =
    pricingBreakdown?.service_tier_downgrade_reason ?? event.service_tier_downgrade_reason;
  const requestedServiceTier = displayRequestedServiceTier(
    pricingBreakdown?.requested_service_tier ?? event.requested_service_tier
  );

  return (
    <div className="row g-3 small">
      <DetailOverview
        event={event}
        serviceTierDowngraded={serviceTierDowngraded}
        requestedServiceTier={requestedServiceTier}
        serviceTierDowngradeReason={serviceTierDowngradeReason}
        actualServiceTier={pricingBreakdown?.service_tier || event.service_tier}
      />
      <ModelCheckSection modelCheck={modelCheck} />
      <PricingBreakdownSection pricingBreakdown={pricingBreakdown} />
    </div>
  );
}

function DetailOverview({
  event,
  serviceTierDowngraded,
  requestedServiceTier,
  serviceTierDowngradeReason,
  actualServiceTier,
}: {
  event: AdminUsageEvent;
  serviceTierDowngraded: boolean;
  requestedServiceTier?: string;
  serviceTierDowngradeReason?: string;
  actualServiceTier?: string;
}) {
  return (
    <>
      <div className="col-12 col-lg-4">
        <div className="text-muted smaller">Event ID</div>
        <div className="font-monospace">{event.id}</div>
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
        <div className="text-muted smaller">实际 Service Tier</div>
        <div className="font-monospace">{serviceTierText(actualServiceTier)}</div>
      </div>
      <div className="col-12 col-lg-4">
        <div className="text-muted smaller">请求 Service Tier</div>
        <div className="font-monospace">{serviceTierText(requestedServiceTier)}</div>
      </div>
      <div className="col-12 col-lg-4">
        <div className="text-muted smaller">降级状态</div>
        <div className={serviceTierDowngraded ? 'text-success fw-bold' : 'font-monospace'}>
          {serviceTierDowngraded ? `已被降级：${serviceTierDowngradeText(serviceTierDowngradeReason)}` : '未降级'}
        </div>
      </div>
    </>
  );
}

function ModelCheckSection({ modelCheck }: { modelCheck?: UsageEventDetail['model_check'] }) {
  if (!modelCheck) return null;

  return (
    <>
      <div className="col-12 col-lg-4">
        <div className="text-muted smaller">转发模型</div>
        <div className="font-monospace text-break">{modelCheck.forwarded_model || '-'}</div>
      </div>
      <div className="col-12 col-lg-4">
        <div className="text-muted smaller">上游返回模型</div>
        <div className="font-monospace text-break">{modelCheck.upstream_response_model || '-'}</div>
      </div>
      <div className="col-12 col-lg-4">
        <div className="text-muted smaller">模型一致性</div>
        <div className={modelCheck.mismatch ? 'text-danger fw-bold' : 'text-success fw-bold'}>
          {modelCheck.mismatch ? '不一致' : '一致'}
        </div>
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
          {cacheActual}) × {formatDecimalPlain(pricingBreakdown.effective_multiplier || '1')} ={' '}
          {formatUSD(pricingBreakdown.final_cost_usd || '0')}{' '}
          <span className="text-muted smaller">
            （倍率: 支付×{formatDecimalPlain(pricingBreakdown.payment_multiplier || '1')} × 渠道组路径(
            {pricingBreakdown.group_name || 'default'})×
            {formatDecimalPlain(pricingBreakdown.group_multiplier || '1')}）
          </span>
        </div>
      </div>
    </div>
  );
}
