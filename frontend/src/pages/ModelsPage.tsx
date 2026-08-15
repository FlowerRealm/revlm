import { useEffect, useMemo, useState } from 'react';

import { listUserModelsDetail, type PluginModel } from '../api/models';
import { SegmentedFrame } from '../components/SegmentedFrame';

// The catalogue is whatever the loaded plugins registered. Pricing keys belong to
// each protocol, so they are listed as they arrive rather than mapped onto a
// fixed input/output/cache shape the core no longer has.
function pricingEntries(model: PluginModel): Array<{ key: string; value: string }> {
  const pricing = model.pricing;
  if (!pricing || typeof pricing !== 'object') return [];
  return Object.entries(pricing)
    .filter(([, value]) => value !== null && value !== undefined && typeof value !== 'object')
    .map(([key, value]) => ({ key, value: String(value) }));
}

export function ModelsPage() {
  const [models, setModels] = useState<PluginModel[]>([]);
  const [loading, setLoading] = useState(true);
  const [err, setErr] = useState('');

  const sortedModels = useMemo(
    () => models.slice().sort((a, b) => String(a.id).localeCompare(String(b.id), 'en-US')),
    [models]
  );

  useEffect(() => {
    (async () => {
      setErr('');
      setLoading(true);
      try {
        const res = await listUserModelsDetail();
        if (!res.success) {
          throw new Error(res.message || '加载失败');
        }
        setModels(res.data || []);
      } catch (e) {
        setErr(e instanceof Error ? e.message : '加载失败');
      } finally {
        setLoading(false);
      }
    })();
  }, []);

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <div className="card overflow-hidden rlm-models-card mb-0">
          <div className="card-body p-0">
            {err ? (
              <div className="alert alert-danger m-3" role="alert">
                <span className="me-2 material-symbols-rounded">report</span> {err}
              </div>
            ) : null}

            {loading ? (
              <div className="text-center py-5 text-muted">加载中…</div>
            ) : sortedModels.length === 0 ? (
              <div className="text-center py-5 text-muted">
                <span className="fs-1 d-block mb-3 material-symbols-rounded">inbox</span>
                暂无可用模型，请联系管理员安装并启用协议插件。
              </div>
            ) : (
              <>
                <div
                  className="d-flex align-items-center justify-content-between p-3 border-bottom bg-white sticky-top"
                  style={{ zIndex: 10 }}
                >
                  <h5 className="mb-0 fs-6 fw-bold text-secondary">
                    <span className="me-2 material-symbols-rounded align-middle">smart_toy</span>可用模型列表
                  </h5>
                  <span className="badge bg-secondary bg-opacity-10 text-secondary border">
                    共 {sortedModels.length} 个模型
                  </span>
                </div>

                <div className="p-3 bg-light bg-opacity-25">
                  <div className="d-flex flex-column gap-2">
                    {sortedModels.map((model) => {
                      const prices = pricingEntries(model);
                      return (
                        <div
                          key={model.id}
                          className="bg-white rounded border p-3 d-flex flex-wrap align-items-center justify-content-between gap-3"
                        >
                          <div className="d-flex flex-column" style={{ minWidth: '200px' }}>
                            <span className="font-monospace fw-bold text-dark fs-6 text-break">{model.id}</span>
                            {model.name && model.name !== model.id ? (
                              <span className="text-muted smaller">{model.name}</span>
                            ) : null}
                          </div>

                          <div className="d-flex flex-wrap align-items-center gap-2 text-secondary small">
                            {prices.length === 0 ? (
                              <span className="text-muted smaller">未提供价格信息</span>
                            ) : (
                              prices.map((price) => (
                                <div
                                  key={price.key}
                                  className="d-flex align-items-baseline gap-1 px-2 py-1 bg-light rounded-pill border"
                                >
                                  <span className="text-muted smaller font-monospace">{price.key}</span>
                                  <span className="font-monospace fw-bold text-dark">{price.value}</span>
                                </div>
                              ))
                            )}
                          </div>
                        </div>
                      );
                    })}
                  </div>
                </div>
              </>
            )}
          </div>
        </div>
      </SegmentedFrame>
    </div>
  );
}
