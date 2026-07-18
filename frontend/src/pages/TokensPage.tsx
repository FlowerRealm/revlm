import { useEffect, useRef, useState } from 'react';

import {
  createUserToken,
  deleteUserToken,
  getUserTokenChannel,
  listUserTokens,
  revealUserToken,
  revokeUserToken,
  rotateUserToken,
  setUserTokenChannel,
  type TokenChannelGroupOption,
  type UserToken,
  type UserTokenChannelGroup,
} from '../api/tokens';
import { getUsageWindows, type UsageWindow } from '../api/usage';
import { BootstrapModal } from '../components/BootstrapModal';
import { DividedStack } from '../components/DividedStack';
import { SegmentedFrame } from '../components/SegmentedFrame';
import { closeModalById } from '../components/modal';
import { formatUSDPlain } from '../format/money';
import { cacheHitRate, formatLocalDate, formatLocalDateTimeMinute } from './usage/usageUtils';

export function TokensPage() {
  const [tokens, setTokens] = useState<UserToken[]>([]);
  const [tokensLoading, setTokensLoading] = useState(true);
  const [tokensErr, setTokensErr] = useState('');
  const [revealed, setRevealed] = useState<Record<number, string>>({});
  const [revealLoading, setRevealLoading] = useState<Record<number, boolean>>({});
  const [copiedID, setCopiedID] = useState<number | null>(null);
  const [groupNameByID, setGroupNameByID] = useState<Record<number, string>>({});

  const [name, setName] = useState('');

  const openGeneratedTokenModalBtnRef = useRef<HTMLButtonElement | null>(null);
  const pendingGeneratedTokenRef = useRef<string | null>(null);
  const [generatedToken, setGeneratedToken] = useState('');
  const [generatedCopied, setGeneratedCopied] = useState(false);

  const openTokenChannelModalBtnRef = useRef<HTMLButtonElement | null>(null);
  const [tokenChannelToken, setTokenChannelToken] = useState<UserToken | null>(null);
  const [tokenChannelData, setTokenChannelData] = useState<UserTokenChannelGroup | null>(null);
  const [selectedGroupID, setSelectedGroupID] = useState(0);
  const [loading, setLoading] = useState(false);
  const [saving, setSaving] = useState(false);
  const [err, setErr] = useState('');
  const [notice, setNotice] = useState('');

  const openTokenUsageModalBtnRef = useRef<HTMLButtonElement | null>(null);
  const [usageToken, setUsageToken] = useState<UserToken | null>(null);
  const [usageWindow, setUsageWindow] = useState<UsageWindow | null>(null);
  const [usageStart, setUsageStart] = useState('');
  const [usageEnd, setUsageEnd] = useState('');
  const [usageLoading, setUsageLoading] = useState(false);
  const [usageErr, setUsageErr] = useState('');

  useEffect(() => {
    if (copiedID == null) return;
    const t = window.setTimeout(() => setCopiedID(null), 2000);
    return () => window.clearTimeout(t);
  }, [copiedID]);

  useEffect(() => {
    if (!generatedCopied) return;
    const t = window.setTimeout(() => setGeneratedCopied(false), 2000);
    return () => window.clearTimeout(t);
  }, [generatedCopied]);

  function rememberGroupNames(groups: TokenChannelGroupOption[]) {
    if (!groups.length) return;
    setGroupNameByID((prev) => {
      const next = { ...prev };
      for (const g of groups) {
        if (g.id > 0 && g.name) next[g.id] = g.name;
      }
      return next;
    });
  }

  function formatTokenChannelGroup(t: UserToken): string {
    const id = t.channel_group_id || 0;
    if (id <= 0) return '-';
    return groupNameByID[id] || `渠道组 #${id}`;
  }

  async function refresh() {
    setTokensErr('');
    setTokensLoading(true);
    try {
      const res = await listUserTokens();
      if (!res.success) {
        throw new Error(res.message || '加载失败');
      }
      const nextTokens = res.data || [];
      setTokens(nextTokens);
      setRevealed((prev) => {
        const active = new Set(nextTokens.filter((t) => t.status === 1).map((t) => t.id));
        const next: Record<number, string> = {};
        for (const [k, v] of Object.entries(prev)) {
          const id = Number(k);
          if (active.has(id)) next[id] = v;
        }
        return next;
      });
      setRevealLoading((prev) => {
        const active = new Set(nextTokens.filter((t) => t.status === 1).map((t) => t.id));
        const next: Record<number, boolean> = {};
        for (const [k, v] of Object.entries(prev)) {
          const id = Number(k);
          if (active.has(id)) next[id] = v;
        }
        return next;
      });
    } catch (e) {
      setTokensErr(e instanceof Error ? e.message : '加载失败');
    } finally {
      setTokensLoading(false);
    }
  }

  function openGeneratedTokenModal(tok: string) {
    setGeneratedCopied(false);
    setGeneratedToken(tok);
    window.setTimeout(() => openGeneratedTokenModalBtnRef.current?.click(), 0);
  }

  async function copyText(raw: string): Promise<boolean> {
    try {
      await navigator.clipboard.writeText(raw);
      return true;
    } catch {
      // fallback
    }
    try {
      const el = document.createElement('textarea');
      el.value = raw;
      el.setAttribute('readonly', 'true');
      el.style.position = 'fixed';
      el.style.top = '0';
      el.style.left = '0';
      el.style.opacity = '0';
      document.body.appendChild(el);
      el.select();
      const ok = document.execCommand('copy');
      document.body.removeChild(el);
      return ok;
    } catch {
      return false;
    }
  }

  async function copyToken(raw: string, tokenID: number) {
    const ok = await copyText(raw);
    if (ok) setCopiedID(tokenID);
  }

  async function revealToken(tokenID: number): Promise<string> {
    if (revealed[tokenID]) return revealed[tokenID];
    setRevealLoading((prev) => ({ ...prev, [tokenID]: true }));
    try {
      const res = await revealUserToken(tokenID);
      if (!res.success) {
        throw new Error(res.message || '查看失败');
      }
      const tok = (res.data?.token || '').toString();
      if (tok.trim() === '') {
        throw new Error('查看失败');
      }
      setRevealed((prev) => ({ ...prev, [tokenID]: tok }));
      return tok;
    } finally {
      setRevealLoading((prev) => ({ ...prev, [tokenID]: false }));
    }
  }

  async function saveTokenChannel() {
    const tokenID = tokenChannelToken?.id || 0;
    if (!tokenID) {
      setErr('未选择 Token');
      setNotice('');
      return;
    }
    if (!selectedGroupID) {
      setErr('请选择一个渠道组');
      setNotice('');
      return;
    }
    setErr('');
    setNotice('');
    setSaving(true);
    try {
      const res = await setUserTokenChannel(tokenID, selectedGroupID);
      if (!res.success) throw new Error(res.message || '保存失败');
      const refreshed = await getUserTokenChannel(tokenID);
      if (refreshed.success) {
        const d = refreshed.data || null;
        setTokenChannelData(d);
        setSelectedGroupID(d?.channel_group_id || 0);
        if (d?.allowed_channel_groups) rememberGroupNames(d.allowed_channel_groups);
      }
      setTokens((prev) => prev.map((t) => (t.id === tokenID ? { ...t, channel_group_id: selectedGroupID } : t)));
      setNotice('已保存');
    } catch (e) {
      setErr(e instanceof Error ? e.message : '保存失败');
    } finally {
      setSaving(false);
    }
  }

  async function refreshUsage(tokenID: number, override?: { start?: string; end?: string }) {
    setUsageErr('');
    setUsageLoading(true);
    try {
      const startValue = (override?.start ?? usageStart).trim();
      const endValue = (override?.end ?? usageEnd).trim();
      const res = await getUsageWindows(startValue || undefined, endValue || undefined, tokenID);
      if (!res.success) throw new Error(res.message || '加载失败');
      const window0 = res.data?.windows?.[0] ?? null;
      setUsageWindow(window0);

      const day0 = window0 ? formatLocalDate(String(window0.since)) : '';
      if (window0) {
        if (!startValue && day0) setUsageStart(day0);
        if (!endValue && (startValue || day0)) setUsageEnd(startValue || day0);
      }
    } catch (e) {
      setUsageErr(e instanceof Error ? e.message : '加载失败');
      setUsageWindow(null);
    } finally {
      setUsageLoading(false);
    }
  }

  async function openTokenUsageModal(t: UserToken) {
    setTokensErr('');
    setUsageToken(t);
    setUsageWindow(null);
    setUsageStart('');
    setUsageEnd('');
    setUsageErr('');
    window.setTimeout(() => openTokenUsageModalBtnRef.current?.click(), 0);
    void refreshUsage(t.id, { start: '', end: '' });
  }

  async function openTokenChannelModal(t: UserToken) {
    setTokensErr('');
    setErr('');
    setNotice('');
    setTokenChannelToken(t);
    setTokenChannelData(null);
    setSelectedGroupID(t.channel_group_id || 0);
    setLoading(true);
    setSaving(false);
    try {
      const res = await getUserTokenChannel(t.id);
      if (!res.success) throw new Error(res.message || '加载失败');
      const d = res.data || null;
      setTokenChannelData(d);
      setSelectedGroupID(d?.channel_group_id || 0);
      if (d?.allowed_channel_groups) rememberGroupNames(d.allowed_channel_groups);
    } catch (e) {
      const msg = e instanceof Error ? e.message : '加载失败';
      setErr(msg);
      setTokenChannelData(null);
    } finally {
      setLoading(false);
      window.setTimeout(() => openTokenChannelModalBtnRef.current?.click(), 0);
    }
  }

  useEffect(() => {
    void refresh();
  }, []);

  const allowedGroups = (tokenChannelData?.allowed_channel_groups || [])
    .slice()
    .sort((a, b) => a.name.localeCompare(b.name, 'zh-CN'));
  const selectedGroup = allowedGroups.find((g) => g.id === selectedGroupID) || null;

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <DividedStack>
          <div className="card mb-0">
            <div className="card-body d-flex flex-column flex-md-row justify-content-between align-items-center">
              <div className="d-flex align-items-center mb-3 mb-md-0">
                <div
                  className="bg-primary bg-opacity-10 text-primary rounded-circle d-flex align-items-center justify-content-center me-3"
                  style={{ width: 48, height: 48 }}
                >
                  <span className="fs-4 material-symbols-rounded">key</span>
                </div>
                <div>
                  <h5 className="mb-1 fw-semibold">我的 API 令牌</h5>
                  <p className="mb-0 text-muted small">
                    为安全起见，令牌默认隐藏；可在此页查看/复制。令牌撤销后无法查看。
                  </p>
                </div>
              </div>
              <button
                type="button"
                className="btn btn-primary btn-sm"
                data-bs-toggle="modal"
                data-bs-target="#createTokenModal"
              >
                <span className="me-1 material-symbols-rounded">add</span> 创建令牌
              </button>
            </div>
          </div>

          {tokensErr ? (
            <div className="alert alert-danger mb-0" role="alert">
              <span className="me-2 material-symbols-rounded">report</span>
              {tokensErr}
            </div>
          ) : null}

          <div className="card h-100 overflow-hidden mb-0">
            <div className="card-body p-0">
              <div className="table-responsive">
                <table className="table table-hover align-middle mb-0">
                  <thead className="bg-light text-muted small text-uppercase">
                    <tr>
                      <th scope="col" className="fw-medium ps-4 py-3">
                        名称
                      </th>
                      <th scope="col" className="fw-medium py-3">
                        渠道组
                      </th>
                      <th scope="col" className="fw-medium py-3">
                        预览
                      </th>
                      <th scope="col" className="fw-medium py-3">
                        状态
                      </th>
                      <th scope="col" className="fw-medium text-end pe-4 py-3">
                        操作
                      </th>
                    </tr>
                  </thead>
                  <tbody className="border-top-0">
                    {tokensLoading ? (
                      <tr>
                        <td colSpan={5} className="text-center py-5 text-muted">
                          加载中…
                        </td>
                      </tr>
                    ) : tokens.length === 0 ? (
                      <tr>
                        <td colSpan={5} className="text-center py-5 text-muted">
                          <div className="mb-2">
                            <span className="fs-3 text-light-emphasis material-symbols-rounded">inbox</span>
                          </div>
                          暂无令牌，点击右上角按钮创建一个。
                        </td>
                      </tr>
                    ) : (
                      tokens.map((t) => (
                        <tr key={t.id}>
                          <td className="ps-4 py-3">
                            {t.name ? (
                              <span className="fw-medium text-dark">{t.name}</span>
                            ) : (
                              <span className="text-muted small fst-italic">无备注</span>
                            )}
                          </td>
                          <td className="py-3">
                            {t.channel_group_id ? (
                              <span className="small text-dark">{formatTokenChannelGroup(t)}</span>
                            ) : (
                              <span className="text-muted small">-</span>
                            )}
                          </td>
                          <td className="py-3">
                            {revealed[t.id] ? (
                              <code className="bg-light px-2 py-1 rounded text-dark border user-select-all">
                                {revealed[t.id]}
                              </code>
                            ) : (
                              <span className="text-muted small">-</span>
                            )}
                          </td>
                          <td className="py-3">
                            {t.status === 1 ? (
                              <span className="badge bg-success bg-opacity-10 text-success rounded-pill px-2">
                                活跃
                              </span>
                            ) : (
                              <span className="badge bg-secondary bg-opacity-10 text-secondary rounded-pill px-2">
                                已撤销
                              </span>
                            )}
                          </td>
                          <td className="text-end pe-4 py-3">
                            {t.status === 1 ? (
                              <>
                                <button
                                  className="btn btn-link text-secondary p-0 text-decoration-none small"
                                  type="button"
                                  disabled={tokensLoading || revealLoading[t.id]}
                                  onClick={async () => {
                                    setTokensErr('');
                                    if (revealed[t.id]) {
                                      setRevealed((prev) => {
                                        const next = { ...prev };
                                        delete next[t.id];
                                        return next;
                                      });
                                      return;
                                    }
                                    try {
                                      await revealToken(t.id);
                                    } catch (e) {
                                      setTokensErr(e instanceof Error ? e.message : '查看失败');
                                    }
                                  }}
                                >
                                  {revealed[t.id] ? '隐藏' : '查看'}
                                </button>

                                <span className="text-muted small mx-2">|</span>

                                <button
                                  className="btn btn-link text-secondary p-0 text-decoration-none small"
                                  type="button"
                                  disabled={tokensLoading || revealLoading[t.id]}
                                  onClick={async () => {
                                    setTokensErr('');
                                    try {
                                      const tok = revealed[t.id] ? revealed[t.id] : await revealToken(t.id);
                                      await copyToken(tok, t.id);
                                    } catch (e) {
                                      setTokensErr(e instanceof Error ? e.message : '复制失败');
                                    }
                                  }}
                                >
                                  {copiedID === t.id ? '已复制' : '复制'}
                                </button>

                                <span className="text-muted small mx-2">|</span>

                                <button
                                  className="btn btn-link text-secondary p-0 text-decoration-none small"
                                  type="button"
                                  disabled={tokensLoading}
                                  onClick={() => void openTokenChannelModal(t)}
                                >
                                  渠道组
                                </button>

                                <span className="text-muted small mx-2">|</span>

                                <button
                                  className="btn btn-link text-secondary p-0 text-decoration-none small"
                                  type="button"
                                  disabled={tokensLoading}
                                  onClick={() => void openTokenUsageModal(t)}
                                >
                                  用量
                                </button>

                                <span className="text-muted small mx-2">|</span>
                              </>
                            ) : null}

                            <button
                              className="btn btn-link text-primary p-0 text-decoration-none small"
                              type="button"
                              disabled={tokensLoading}
                              onClick={async () => {
                                setTokensErr('');
                                setRevealed((prev) => {
                                  const next = { ...prev };
                                  delete next[t.id];
                                  return next;
                                });
                                try {
                                  const res = await rotateUserToken(t.id);
                                  if (!res.success) {
                                    throw new Error(res.message || '重新生成失败');
                                  }
                                  const tok = res.data?.token;
                                  await refresh();
                                  if (tok) openGeneratedTokenModal(tok);
                                } catch (e) {
                                  setTokensErr(e instanceof Error ? e.message : '重新生成失败');
                                }
                              }}
                            >
                              重新生成
                            </button>

                            <span className="text-muted small mx-2">|</span>

                            {t.status === 1 ? (
                              <button
                                className="btn btn-link text-danger p-0 text-decoration-none small"
                                type="button"
                                disabled={tokensLoading}
                                onClick={async () => {
                                  setTokensErr('');
                                  try {
                                    const res = await revokeUserToken(t.id);
                                    if (!res.success) {
                                      throw new Error(res.message || '撤销失败');
                                    }
                                    await refresh();
                                  } catch (e) {
                                    setTokensErr(e instanceof Error ? e.message : '撤销失败');
                                  }
                                }}
                              >
                                撤销
                              </button>
                            ) : (
                              <button
                                className="btn btn-link text-danger p-0 text-decoration-none small"
                                type="button"
                                disabled={tokensLoading}
                                onClick={async () => {
                                  setTokensErr('');
                                  try {
                                    const res = await deleteUserToken(t.id);
                                    if (!res.success) {
                                      throw new Error(res.message || '删除失败');
                                    }
                                    await refresh();
                                  } catch (e) {
                                    setTokensErr(e instanceof Error ? e.message : '删除失败');
                                  }
                                }}
                              >
                                删除
                              </button>
                            )}
                          </td>
                        </tr>
                      ))
                    )}
                  </tbody>
                </table>
              </div>
            </div>
          </div>
        </DividedStack>
      </SegmentedFrame>

      {/* programmatically open the generated-token modal */}
      <button
        ref={openGeneratedTokenModalBtnRef}
        type="button"
        className="d-none"
        data-bs-toggle="modal"
        data-bs-target="#generatedTokenModal"
      ></button>

      {/* programmatically open the token-channel modal */}
      <button
        ref={openTokenChannelModalBtnRef}
        type="button"
        className="d-none"
        data-bs-toggle="modal"
        data-bs-target="#tokenChannelModal"
      ></button>

      {/* programmatically open the token-usage modal */}
      <button
        ref={openTokenUsageModalBtnRef}
        type="button"
        className="d-none"
        data-bs-toggle="modal"
        data-bs-target="#tokenUsageModal"
      ></button>

      <BootstrapModal
        id="tokenUsageModal"
        title="令牌用量"
        dialogClassName="modal-dialog-centered modal-lg"
        onHidden={() => {
          setUsageToken(null);
          setUsageWindow(null);
          setUsageStart('');
          setUsageEnd('');
          setUsageLoading(false);
          setUsageErr('');
        }}
      >
        {usageToken ? (
          <div>
            <div className="mb-3">
              <div className="text-muted small mb-1">令牌</div>
              <div className="fw-semibold">{(usageToken.name || `Token #${usageToken.id}`).toString()}</div>
            </div>

            {usageErr ? (
              <div className="alert alert-danger mb-3" role="alert">
                <span className="me-2 material-symbols-rounded">warning</span>
                {usageErr}
              </div>
            ) : null}

            <form
              className="row g-2 align-items-end mb-3"
              onSubmit={(e) => {
                e.preventDefault();
                void refreshUsage(usageToken.id);
              }}
            >
              <div className="col-auto">
                <label className="form-label small text-muted mb-1">开始日期</label>
                <input
                  className="form-control form-control-sm"
                  type="date"
                  value={usageStart}
                  onChange={(e) => setUsageStart(e.target.value)}
                  disabled={usageLoading}
                />
              </div>
              <div className="col-auto">
                <label className="form-label small text-muted mb-1">结束日期</label>
                <input
                  className="form-control form-control-sm"
                  type="date"
                  value={usageEnd}
                  onChange={(e) => setUsageEnd(e.target.value)}
                  disabled={usageLoading}
                />
              </div>
              <div className="col-auto d-flex gap-2">
                <button className="btn btn-sm btn-primary" type="submit" disabled={usageLoading}>
                  查询
                </button>
                <button
                  className="btn btn-sm btn-white border text-dark"
                  type="button"
                  disabled={usageLoading}
                  onClick={() => {
                    setUsageStart('');
                    setUsageEnd('');
                    void refreshUsage(usageToken.id, { start: '', end: '' });
                  }}
                >
                  重置
                </button>
              </div>
            </form>

            {usageLoading ? <div className="text-muted">加载中…</div> : null}

            {usageWindow ? (
              <div className="table-responsive">
                <table className="table table-sm mb-0">
                  <tbody>
                    <tr>
                      <td className="text-muted">时间范围</td>
                      <td className="text-end">
                        {formatLocalDateTimeMinute(String(usageWindow.since))} -{' '}
                        {formatLocalDateTimeMinute(String(usageWindow.until))}
                      </td>
                    </tr>
                    <tr>
                      <td className="text-muted">消耗 (USD)</td>
                      <td className="text-end">{formatUSDPlain(usageWindow.usd)}</td>
                    </tr>
                    <tr>
                      <td className="text-muted">请求数</td>
                      <td className="text-end">{usageWindow.requests}</td>
                    </tr>
                    <tr>
                      <td className="text-muted">Tokens</td>
                      <td className="text-end">{usageWindow.tokens}</td>
                    </tr>
                    <tr>
                      <td className="text-muted">Tokens</td>
                      <td className="text-end">
                        {usageWindow.input_tokens}/{usageWindow.output_tokens}/
                        {usageWindow.cache_read_tokens + usageWindow.cache_creation_tokens}
                      </td>
                    </tr>
                    <tr>
                      <td className="text-muted">缓存率</td>
                      <td className="text-end">{cacheHitRate(usageWindow.cache_ratio)}</td>
                    </tr>
                    <tr>
                      <td className="text-muted">RPM/TPM</td>
                      <td className="text-end">
                        {usageWindow.rpm}/{usageWindow.tpm}
                      </td>
                    </tr>
                  </tbody>
                </table>
              </div>
            ) : null}
          </div>
        ) : (
          <div className="text-muted">请选择一个令牌。</div>
        )}
      </BootstrapModal>

      <BootstrapModal
        id="generatedTokenModal"
        title="令牌已生成"
        dialogClassName="modal-dialog-centered"
        onHidden={() => {
          setGeneratedCopied(false);
          setGeneratedToken('');
        }}
      >
        <div className="alert alert-warning border-0 bg-warning bg-opacity-10 d-flex align-items-start mb-3">
          <span className="me-2 mt-1 material-symbols-rounded">warning</span>
          <div className="small">请复制并妥善保存。也可以在令牌列表页查看/复制（默认隐藏）。令牌撤销后无法查看。</div>
        </div>

        <div className="mb-3">
          <label className="form-label small fw-bold text-uppercase text-muted">API 令牌</label>
          <div className="input-group input-group-lg">
            <input
              type="text"
              className="form-control font-monospace bg-light border-end-0"
              value={generatedToken}
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
              className={`btn ${generatedCopied ? 'btn-success text-white' : 'btn-light'} border border-start-0 px-4`}
              type="button"
              title="点击复制"
              disabled={generatedToken.trim() === ''}
              onClick={async () => {
                setTokensErr('');
                const ok = await copyText(generatedToken);
                if (!ok) {
                  setTokensErr('复制失败');
                  return;
                }
                setGeneratedCopied(true);
              }}
            >
              <span className="material-symbols-rounded">{generatedCopied ? 'check' : 'content_copy'}</span>
            </button>
          </div>
          <div
            className={`text-success small mt-2 opacity-0 transition-opacity${generatedCopied ? ' opacity-100' : ''}`}
          >
            <span className="me-1 material-symbols-rounded">check</span>已成功复制到剪贴板
          </div>
        </div>

        <div className="modal-footer border-top-0 px-0 pb-0">
          <button type="button" className="btn btn-light" data-bs-dismiss="modal">
            关闭
          </button>
        </div>
      </BootstrapModal>

      <BootstrapModal
        id="tokenChannelModal"
        title={
          tokenChannelToken
            ? `渠道组：${(tokenChannelToken.name || '').trim() || `Token #${tokenChannelToken.id}`}`
            : '渠道组'
        }
        dialogClassName="modal-dialog-centered"
        onHidden={() => {
          setTokenChannelToken(null);
          setTokenChannelData(null);
          setSelectedGroupID(0);
          setErr('');
          setNotice('');
          setLoading(false);
          setSaving(false);
        }}
      >
        {!tokenChannelToken ? (
          <div className="text-muted">未选择 Token。</div>
        ) : (
          <div>
            {err ? (
              <div className="alert alert-danger d-flex align-items-center" role="alert">
                <span className="me-2 material-symbols-rounded">warning</span>
                <div>{err}</div>
              </div>
            ) : null}

            {notice ? (
              <div className="alert alert-success d-flex align-items-center" role="alert">
                <span className="me-2 material-symbols-rounded">check_circle</span>
                <div>{notice}</div>
              </div>
            ) : null}

            <p className="text-muted small mb-3">为该令牌指定一个渠道组。上游失败时会在组内按顺序切换渠道。</p>

            {loading ? <div className="text-muted small mb-2">加载中…</div> : null}

            <div className="mb-3">
              <label className="form-label fw-medium text-dark">渠道组</label>
              <select
                className="form-select"
                value={selectedGroupID || ''}
                onChange={(e) => setSelectedGroupID(Number(e.target.value) || 0)}
                disabled={loading || saving}
              >
                <option value="">选择渠道组…</option>
                {allowedGroups.map((g) => (
                  <option key={g.id} value={g.id} disabled={!g.status}>
                    {g.name}
                    {g.price_multiplier ? ` · x${g.price_multiplier}` : ''}
                    {!g.status ? '（禁用）' : ''}
                  </option>
                ))}
              </select>
              {selectedGroup ? (
                <div className="form-text text-muted">
                  {selectedGroup.description ? `${selectedGroup.description} · ` : ''}
                  {selectedGroup.price_multiplier ? `倍率 x${selectedGroup.price_multiplier}` : '倍率 x1'}
                  {!selectedGroup.status ? ' · 当前禁用' : ''}
                </div>
              ) : (
                <div className="form-text text-muted">请从可用渠道组中选择一个。</div>
              )}
            </div>

            <div className="d-grid d-md-flex justify-content-md-end gap-2">
              <button type="button" className="btn btn-light" data-bs-dismiss="modal" disabled={saving}>
                关闭
              </button>
              <button
                type="button"
                className="btn btn-primary px-4"
                disabled={saving || loading || !selectedGroupID}
                onClick={() => void saveTokenChannel()}
              >
                {saving ? '保存中…' : '保存'}
              </button>
            </div>
          </div>
        )}
      </BootstrapModal>

      <BootstrapModal
        id="createTokenModal"
        title="创建新 API 令牌"
        dialogClassName="modal-dialog-centered"
        headerClassName="border-bottom-0 pb-0"
        bodyClassName="pt-4"
        onHidden={() => {
          setName('');
          const tok = pendingGeneratedTokenRef.current;
          pendingGeneratedTokenRef.current = null;
          if (tok) openGeneratedTokenModal(tok);
        }}
      >
        <form
          onSubmit={async (e) => {
            e.preventDefault();
            setTokensErr('');
            try {
              const res = await createUserToken(name.trim() || undefined);
              if (!res.success) {
                throw new Error(res.message || '创建失败');
              }
              const tok = res.data?.token || '';
              pendingGeneratedTokenRef.current = tok.trim() === '' ? null : tok;
              setName('');
              closeModalById('createTokenModal');
              await refresh();
            } catch (e) {
              setTokensErr(e instanceof Error ? e.message : '创建失败');
            }
          }}
        >
          <div className="mb-3">
            <label className="form-label fw-medium text-dark">备注名称</label>
            <input
              name="name"
              type="text"
              className="form-control"
              placeholder="例如：我的项目、笔记本 CLI…"
              autoFocus
              value={name}
              onChange={(e) => setName(e.target.value)}
            />
            <div className="form-text text-muted">给令牌起个名字，方便日后管理。</div>
          </div>
          <div className="alert alert-light border mb-0 d-flex align-items-start small">
            <span className="text-primary me-2 mt-1 material-symbols-rounded">info</span>
            <div>创建成功后会弹窗展示一次；也可以在列表页查看/复制（默认隐藏）。令牌撤销后无法查看。</div>
          </div>
          <div className="modal-footer border-top-0 px-0">
            <button type="button" className="btn btn-light text-muted" data-bs-dismiss="modal">
              取消
            </button>
            <button type="submit" className="btn btn-primary px-4" disabled={tokensLoading}>
              创建
            </button>
          </div>
        </form>
      </BootstrapModal>
    </div>
  );
}
