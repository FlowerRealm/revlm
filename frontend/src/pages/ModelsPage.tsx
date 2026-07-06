import { useEffect, useMemo, useState } from 'react';

import { listUserModelsDetail, type UserManagedModel } from '../api/models';
import { SegmentedFrame } from '../components/SegmentedFrame';
import { formatUSDPlain } from '../format/money';
import { providerCachePriceRows } from '../modelPricingDisplay';

export function ModelsPage() {
  const [models, setModels] = useState<UserManagedModel[]>([]);
  const [loading, setLoading] = useState(true);
  const [err, setErr] = useState('');
  const [activeGroup, setActiveGroup] = useState<string>('default');

  const groupedModels = useMemo(() => {
    const buckets = new Map<string, UserManagedModel[]>();
    for (const model of models) {
      const ownedBy = (model.owned_by || '').trim() || 'unknown';
      const normalized: UserManagedModel = {
        ...model,
        group_name: (model.group_name || '').trim(),
        owned_by: ownedBy,
      };
      if (!buckets.has(ownedBy)) {
        buckets.set(ownedBy, []);
      }
      buckets.get(ownedBy)!.push(normalized);
    }

    return Array.from(buckets.entries())
      .map(([ownedBy, groupItems]) => {
        const sortedModels = groupItems.slice().sort((a, b) => {
          return a.public_id.localeCompare(b.public_id, 'en-US');
        });
        return {
          groupName: ownedBy,
          displayName: ownedBy,
          models: sortedModels,
        };
      })
      .sort((a, b) => {
        if (a.groupName === 'unknown' && b.groupName !== 'unknown') return 1;
        if (b.groupName === 'unknown' && a.groupName !== 'unknown') return -1;
        return a.groupName.localeCompare(b.groupName, 'zh-CN');
      });
  }, [models]);

  useEffect(() => {
    if (groupedModels.length === 0) return;
    if (groupedModels.some((g) => g.groupName === activeGroup)) return;
    setActiveGroup(groupedModels[0].groupName);
  }, [groupedModels, activeGroup]);

  const currentGroup = useMemo(() => {
    if (groupedModels.length === 0) return null;
    return groupedModels.find((g) => g.groupName === activeGroup) || groupedModels[0];
  }, [groupedModels, activeGroup]);

  const currentGroupByOwner = useMemo(() => {
    if (!currentGroup) return [];
    const buckets = new Map<string, UserManagedModel[]>();
    for (const model of currentGroup.models) {
      const ownedBy = (model.owned_by || '').trim() || 'unknown';
      if (!buckets.has(ownedBy)) {
        buckets.set(ownedBy, []);
      }
      buckets.get(ownedBy)!.push({
        ...model,
        owned_by: ownedBy,
      });
    }

    return Array.from(buckets.entries())
      .map(([ownedBy, ownerModels]) => ({
        ownedBy,
        models: ownerModels.slice().sort((a, b) => a.public_id.localeCompare(b.public_id, 'en-US')),
      }))
      .sort((a, b) => {
        if (a.ownedBy === 'unknown' && b.ownedBy !== 'unknown') return 1;
        if (b.ownedBy === 'unknown' && a.ownedBy !== 'unknown') return -1;
        return a.ownedBy.localeCompare(b.ownedBy, 'en-US');
      });
  }, [currentGroup]);

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
            ) : models.length === 0 ? (
              <div className="text-center py-5 text-muted">
                <span className="fs-1 d-block mb-3 material-symbols-rounded">inbox</span>
                暂无可用模型，请联系管理员配置模型目录。
              </div>
            ) : (
              <div className="rlm-models-layout">
                <aside className="rlm-models-groups">
                  <div className="rlm-models-groups-head">
                    <span className="material-symbols-rounded">dataset</span> 归属方
                  </div>
                  <div className="rlm-models-group-list">
                    {groupedModels.map((group) => (
                      <button
                        key={group.groupName}
                        type="button"
                        className={`rlm-models-group-item ${activeGroup === group.groupName ? 'active' : ''}`}
                        onClick={() => setActiveGroup(group.groupName)}
                      >
                        <span className="rlm-models-group-name font-monospace">{group.displayName}</span>
                        <span className="rlm-models-group-count">{group.models.length}</span>
                      </button>
                    ))}
                  </div>
                </aside>
                <section className="rlm-models-main d-flex flex-column">
                  <div
                    className="d-flex align-items-center justify-content-between p-3 border-bottom bg-white sticky-top"
                    style={{ zIndex: 10 }}
                  >
                    <h5 className="mb-0 fs-6 fw-bold text-secondary">
                      <span className="me-2 material-symbols-rounded align-middle">smart_toy</span>可用模型列表
                    </h5>
                    <span className="badge bg-secondary bg-opacity-10 text-secondary border">
                      共 {currentGroup?.models.length || 0} 个模型
                    </span>
                  </div>

                  <div className="p-3 bg-light bg-opacity-25 flex-fill overflow-auto">
                    {(currentGroup?.models || []).length === 0 ? (
                      <div className="text-center py-5 text-muted">
                        <span className="fs-1 d-block mb-3 material-symbols-rounded">folder_off</span>
                        当前归属方视图暂无可用模型。
                      </div>
                    ) : (
                      <div className="d-flex flex-column gap-2">
                        {currentGroupByOwner.map((ownerGroup) => (
                          <div
                            key={`${currentGroup?.groupName}:${ownerGroup.ownedBy}`}
                            className="d-flex flex-column gap-2"
                          >
                            {ownerGroup.models.map((m) => {
                              const cachePriceRows = providerCachePriceRows(m.owned_by, m).filter(
                                (row) => parseFloat(row.value) > 0
                              );
                              return (
                                <div
                                  key={m.public_id}
                                  className="bg-white rounded border p-3 transition-all hover-shadow d-flex flex-wrap align-items-center justify-content-between gap-3"
                                >
                                  {/* Left: Identity */}
                                  <div className="d-flex align-items-center gap-3" style={{ minWidth: '200px' }}>
                                    {m.icon_url ? (
                                      <img
                                        className="rlm-model-icon rounded-3"
                                        src={m.icon_url}
                                        alt={m.owned_by || 'revlm'}
                                        title={m.owned_by || 'revlm'}
                                        loading="lazy"
                                        style={{ width: '32px', height: '32px' }}
                                        onError={(e) => {
                                          (e.currentTarget as HTMLImageElement).style.display = 'none';
                                        }}
                                      />
                                    ) : (
                                      <div
                                        className="d-flex align-items-center justify-content-center bg-secondary bg-opacity-10 rounded-3 text-secondary"
                                        style={{ width: '32px', height: '32px' }}
                                      >
                                        <span className="material-symbols-rounded" style={{ fontSize: '20px' }}>
                                          smart_toy
                                        </span>
                                      </div>
                                    )}
                                    <div className="d-flex flex-column">
                                      <span className="font-monospace fw-bold text-dark fs-6 text-break">
                                        {m.public_id}
                                      </span>
                                      <span className="text-muted smaller" style={{ fontSize: '0.75rem' }}>
                                        {m.owned_by || 'Unknown'}
                                      </span>
                                    </div>
                                  </div>

                                  {/* Middle: Pricing */}
                                  <div className="d-flex flex-wrap align-items-center gap-3 text-secondary small flex-grow-1 justify-content-end justify-content-lg-center">
                                    {/* Normal Pricing */}
                                    <div className="d-flex align-items-center gap-3 px-2 py-1 bg-light rounded-pill border">
                                      <div className="d-flex align-items-baseline gap-1">
                                        <span className="text-muted smaller">In</span>
                                        <span className="font-monospace fw-bold text-dark">
                                          ${formatUSDPlain(m.input_usd_per_1m)}
                                        </span>
                                      </div>
                                      <div className="vr opacity-25"></div>
                                      <div className="d-flex align-items-baseline gap-1">
                                        <span className="text-muted smaller">Out</span>
                                        <span className="font-monospace fw-bold text-dark">
                                          ${formatUSDPlain(m.output_usd_per_1m)}
                                        </span>
                                      </div>
                                    </div>

                                    {cachePriceRows.length > 0 && (
                                      <div className="d-flex align-items-center gap-3 px-2 py-1 bg-warning bg-opacity-10 rounded-pill border border-warning-subtle">
                                        {cachePriceRows.map((row, index) => (
                                          <div className="d-flex align-items-center gap-3" key={row.key}>
                                            {index > 0 ? <div className="vr opacity-25"></div> : null}
                                            <div className="d-flex align-items-baseline gap-1">
                                              <span className="text-warning-emphasis smaller">{row.shortLabel}</span>
                                              <span className="font-monospace fw-bold text-dark">
                                                ${formatUSDPlain(row.value)}
                                              </span>
                                            </div>
                                          </div>
                                        ))}
                                      </div>
                                    )}
                                  </div>

                                  {/* Right: Status */}
                                  <div className="d-none d-md-block ps-2">
                                    <span className="badge bg-success bg-opacity-10 text-success border border-success-subtle rounded-pill px-3 py-2 fw-normal">
                                      可用
                                    </span>
                                  </div>
                                </div>
                              );
                            })}
                          </div>
                        ))}
                      </div>
                    )}
                  </div>
                </section>
              </div>
            )}
          </div>
        </div>
      </SegmentedFrame>
    </div>
  );
}
