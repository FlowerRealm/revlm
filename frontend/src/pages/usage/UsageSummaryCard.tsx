import type { UsageWindow } from '../../api/usage';
import { formatSecondsFromMilliseconds } from '../../format/duration';
import { formatIntComma } from '../../format/int';
import { formatUSDPlain } from '../../format/money';
import { cacheHitRate } from './usageUtils';

export function UsageSummaryCard({
  data,
  rangeSinceText,
  rangeUntilText,
}: {
  data: UsageWindow;
  rangeSinceText: string;
  rangeUntilText: string;
}) {
  const rpm = formatIntComma(data.rpm ?? 0);
  const tpm = formatIntComma(data.tpm ?? 0);
  const cachedTotal = data.cache_read_tokens + data.cache_creation_tokens;
  const cachedTotalText = formatIntComma(cachedTotal);
  const tokensPerSecond = data.tokens_per_second > 0 ? data.tokens_per_second.toFixed(2) : '-';

  return (
    <div className="card border-0 overflow-hidden">
      <div className="bg-primary bg-opacity-10 py-3 px-4 d-flex justify-content-between align-items-center">
        <div>
          <span className="text-primary fw-bold text-uppercase small">统计区间</span>
          <span className="text-primary text-opacity-75 smaller ms-2">
            统计区间: {rangeSinceText} ~ {rangeUntilText}
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
              <h1 className="display-6 fw-bold mb-0 text-dark">{formatUSDPlain(data.usd)}</h1>
            </div>
          </div>
          <div className="col-lg-8 ps-lg-4">
            <div className="row g-3">
              <div className="col-sm-6 col-md-3">
                <div className="metric-card p-3 rounded-3 border">
                  <div className="text-muted smaller mb-1">全局请求数</div>
                  <div className="h4 fw-bold mb-1">{formatIntComma(data.requests)}</div>
                  <div className="text-primary smaller fw-medium">{rpm} RPM</div>
                </div>
              </div>
              <div className="col-sm-6 col-md-3">
                <div className="metric-card p-3 rounded-3 border">
                  <div className="text-muted smaller mb-1">Token 吞吐</div>
                  <div className="h4 fw-bold mb-1">{formatIntComma(data.tokens)}</div>
                  <div className="text-primary smaller fw-medium">{tpm} TPM</div>
                </div>
              </div>
              <div className="col-sm-6 col-md-3">
                <div className="metric-card p-3 rounded-3 border">
                  <div className="text-muted smaller mb-1">缓存率</div>
                  <div className="h4 fw-bold mb-1">{cacheHitRate(data.cache_ratio)}</div>
                  <div className="text-muted smaller fw-medium">输入 + 输出</div>
                </div>
              </div>
              <div className="col-sm-6 col-md-3">
                <div className="metric-card p-3 rounded-3 border">
                  <div className="text-muted smaller mb-1">缓存 Token</div>
                  <div className="h4 fw-bold mb-1">{cachedTotalText}</div>
                  <div className="text-muted smaller fw-medium">输入 + 输出</div>
                </div>
              </div>
              <div className="col-sm-6 col-md-3">
                <div className="metric-card p-3 rounded-3 border">
                  <div className="text-muted smaller mb-1">平均首字延迟</div>
                  <div className="h4 fw-bold mb-1">{formatSecondsFromMilliseconds(data.avg_first_token_latency)}</div>
                  <div className="text-muted smaller fw-medium">基于有效首字样本</div>
                </div>
              </div>
              <div className="col-sm-6 col-md-3">
                <div className="metric-card p-3 rounded-3 border">
                  <div className="text-muted smaller mb-1">平均 Tokens/s</div>
                  <div className="h4 fw-bold mb-1">{tokensPerSecond}</div>
                  <div className="text-muted smaller fw-medium">输出 Token 解码速率</div>
                </div>
              </div>
              <div className="col-12 mt-3">
                <div className="bg-light p-3 rounded-3">
                  <div className="row text-center small">
                    <div className="col-6 border-end">
                      <div className="text-muted smaller">输入总计</div>
                      <div className="fw-medium">{formatIntComma(data.input_tokens)}</div>
                    </div>
                    <div className="col-6">
                      <div className="text-muted smaller">输出总计</div>
                      <div className="fw-medium">{formatIntComma(data.output_tokens)}</div>
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
