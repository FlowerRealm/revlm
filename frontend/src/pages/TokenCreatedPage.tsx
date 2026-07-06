import { useEffect, useMemo, useState } from 'react';
import { useLocation } from 'react-router-dom';

import { SegmentedFrame } from '../components/SegmentedFrame';

type LocationState = {
  token?: string;
};

export function TokenCreatedPage() {
  const location = useLocation();

  const token = useMemo(() => {
    const st = location.state as LocationState | null;
    return (st?.token || '').toString();
  }, [location.state]);

  const [copied, setCopied] = useState(false);

  useEffect(() => {
    if (!copied) return;
    const t = window.setTimeout(() => setCopied(false), 2000);
    return () => window.clearTimeout(t);
  }, [copied]);

  const canCopy = token.trim() !== '';

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <div className="row justify-content-center mb-0">
          <div className="col-lg-7 col-xl-6">
            <div className="card mt-4 mb-0">
              <div className="card-body p-4 p-md-5">
                <div className="text-center mb-4">
                  <div
                    className="bg-success bg-opacity-10 text-success rounded-circle d-inline-flex align-items-center justify-content-center mb-3"
                    style={{ width: 64, height: 64 }}
                  >
                    <span className="fs-1 material-symbols-rounded">check</span>
                  </div>
                  <h4 className="fw-bold text-dark">令牌已生成</h4>
                  <p className="text-muted">这是您的新 API 令牌，请立即复制并妥善保存。</p>
                </div>

                <div className="alert alert-warning border-0 bg-warning bg-opacity-10 d-flex align-items-start mb-4">
                  <span className="me-2 mt-1 material-symbols-rounded">warning</span>
                  <div className="small">
                    <strong>安全提醒：</strong>
                    令牌会保存在服务端。为安全起见，在令牌列表页默认隐藏；需要使用时再查看/复制。令牌撤销后无法查看。
                  </div>
                </div>

                {!canCopy ? (
                  <div className="alert alert-danger d-flex align-items-center" role="alert">
                    <span className="me-2 material-symbols-rounded">warning</span>
                    <div>令牌不存在。请回到令牌列表重新生成。</div>
                  </div>
                ) : (
                  <div className="mb-4">
                    <label className="form-label small fw-bold text-uppercase text-muted">API 令牌</label>
                    <div className="input-group input-group-lg">
                      <input
                        id="tokenInput"
                        type="text"
                        className="form-control font-monospace bg-light border-end-0 rlm-token-input"
                        value={token}
                        readOnly
                        onClick={(e) => {
                          try {
                            e.currentTarget.select();
                          } catch {
                            // ignore
                          }
                        }}
                      />
                      <button
                        id="copyTokenBtn"
                        className={`btn ${copied ? 'btn-success text-white' : 'btn-light'} border border-start-0 px-4`}
                        type="button"
                        title="点击复制"
                        onClick={async () => {
                          try {
                            await navigator.clipboard.writeText(token);
                            setCopied(true);
                          } catch {
                            // fallback
                            try {
                              const input = document.getElementById('tokenInput') as HTMLInputElement | null;
                              if (!input) return;
                              input.select();
                              const ok = document.execCommand('copy');
                              if (ok) setCopied(true);
                            } catch {
                              // ignore
                            }
                          }
                        }}
                      >
                        <span className="material-symbols-rounded">{copied ? 'check' : 'content_copy'}</span>
                      </button>
                    </div>
                    <div
                      id="copyFeedback"
                      className={`text-success small mt-2 opacity-0 transition-opacity${copied ? ' opacity-100' : ''}`}
                    >
                      <span className="me-1 material-symbols-rounded">check</span>已成功复制到剪贴板
                    </div>
                  </div>
                )}

                <div className="text-muted small text-center">您可以在左侧菜单进入「API 令牌」继续管理。</div>
              </div>
            </div>
          </div>
        </div>
      </SegmentedFrame>
    </div>
  );
}
