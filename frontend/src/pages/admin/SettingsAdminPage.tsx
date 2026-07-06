import { useCallback, useEffect, useState } from 'react';

import { getAdminSettings, type AdminSettings, type UpdateAdminSettingsRequest } from '../../api/admin/settings';
import { DividedStack } from '../../components/DividedStack';
import { SegmentedFrame } from '../../components/SegmentedFrame';
import { AdminConfigSection } from '../../components/admin/AdminConfigWorkspace';
import { adminSettingsTemplate } from '../../components/admin/configTemplates';

function initForm(s: AdminSettings): UpdateAdminSettingsRequest {
  return {
    site_base_url: s.site_base_url || '',
    billing_paygo_price_multiplier: s.billing_paygo_price_multiplier || '1',
  };
}

export function SettingsAdminPage() {
  const [settings, setSettings] = useState<AdminSettings | null>(null);
  const [form, setForm] = useState<UpdateAdminSettingsRequest | null>(null);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [err, setErr] = useState('');
  const [notice, setNotice] = useState('');

  const refresh = useCallback(async () => {
    setErr('');
    setNotice('');
    setLoading(true);
    try {
      const res = await getAdminSettings();
      if (!res.success) throw new Error(res.message || '加载失败');
      const nextSettings = res.data || null;
      setSettings(nextSettings);
      setForm(nextSettings ? initForm(nextSettings) : null);
    } catch (e) {
      setErr(e instanceof Error ? e.message : '加载失败');
      setSettings(null);
      setForm(null);
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  async function saveSettings() {
    if (!form) return;
    setSaving(true);
    setErr('');
    setNotice('');
    try {
      await adminSettingsTemplate.save(form);
      const res = await getAdminSettings();
      if (!res.success) throw new Error(res.message || '加载失败');
      const nextSettings = res.data || null;
      setSettings(nextSettings);
      setForm(nextSettings ? initForm(nextSettings) : null);
      setNotice('已保存');
    } catch (e) {
      setErr(e instanceof Error ? e.message : '保存失败');
    } finally {
      setSaving(false);
    }
  }

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <DividedStack>
          <div className="card mb-0">
            <div className="card-body d-flex flex-column flex-lg-row justify-content-between align-items-lg-center gap-3">
              <div className="d-flex align-items-center">
                <div
                  className="bg-secondary bg-opacity-10 text-secondary rounded-circle d-flex align-items-center justify-content-center me-3"
                  style={{ width: 48, height: 48 }}
                >
                  <span className="fs-4 material-symbols-rounded">tune</span>
                </div>
                <div>
                  <h5 className="mb-1 fw-semibold">系统设置</h5>
                  <p className="mb-0 text-muted small">保持原来的紧凑卡片布局，但把配置项合并到同一页连续展示。</p>
                </div>
              </div>
            </div>
          </div>

          {notice ? (
            <div className="alert alert-success d-flex align-items-center mb-0" role="alert">
              <span className="me-2 material-symbols-rounded">check_circle</span>
              <div>{notice}</div>
            </div>
          ) : null}

          {err ? (
            <div className="alert alert-danger d-flex align-items-center mb-0" role="alert">
              <span className="me-2 material-symbols-rounded">warning</span>
              <div>{err}</div>
            </div>
          ) : null}

          {loading ? (
            <div className="text-muted">加载中…</div>
          ) : !settings || !form ? (
            <div className="alert alert-warning mb-0">未找到设置。</div>
          ) : (
            <div className="mx-auto w-100" style={{ maxWidth: 1120 }}>
              <div className="card mb-0">
                <form
                  className="card-body d-flex flex-column gap-4"
                  onSubmit={(e) => {
                    e.preventDefault();
                    void saveSettings();
                  }}
                >
                  <AdminConfigSection id="settings-base" title="基础设置" description="站点基础行为。">
                    <div className="card">
                      <div className="card-body">
                        <h5 className="fw-semibold mb-3">基础设置</h5>

                        <div className="mb-3">
                          <label className="form-label fw-medium d-flex justify-content-between">
                            <span>Site Base URL</span>
                            {settings.site_base_url_override ? (
                              <span className="badge bg-light text-dark border">数据库覆盖</span>
                            ) : (
                              <span className="badge bg-light text-dark border">请求推导</span>
                            )}
                          </label>
                          <input
                            className="form-control"
                            value={form.site_base_url}
                            onChange={(e) => setForm({ ...form, site_base_url: e.target.value })}
                            placeholder="https://example.com"
                            disabled={saving}
                          />
                          <div className="form-text small text-muted">
                            生效值：<code className="user-select-all">{settings.site_base_url_effective}</code>
                            {settings.site_base_url_invalid ? (
                              <span className="badge bg-warning ms-2">不合法</span>
                            ) : null}
                          </div>
                        </div>
                      </div>
                    </div>
                  </AdminConfigSection>

                  <AdminConfigSection id="settings-billing" title="计费" description="配置按量计费相关参数。">
                    <div className="card">
                      <div className="card-body">
                        <h5 className="fw-semibold mb-3">计费</h5>

                        <div className="row g-3">
                          <div className="col-md-6">
                            <label className="form-label fw-medium d-flex justify-content-between">
                              <span>按量计费倍率</span>
                              {settings.billing_paygo_price_multiplier_override ? (
                                <span className="badge bg-light text-dark border">数据库覆盖</span>
                              ) : null}
                            </label>
                            <div className="input-group">
                              <span className="input-group-text">×</span>
                              <input
                                className="form-control"
                                value={form.billing_paygo_price_multiplier}
                                onChange={(e) => setForm({ ...form, billing_paygo_price_multiplier: e.target.value })}
                                placeholder="1"
                                inputMode="decimal"
                                disabled={saving}
                              />
                            </div>
                            <div className="form-text small text-muted">
                              最终计费倍率 = PayGO 倍率 × 最终命中渠道组路径倍率。
                            </div>
                          </div>
                        </div>
                      </div>
                    </div>
                  </AdminConfigSection>

                  <div className="d-flex justify-content-end">
                    <button type="submit" className="btn btn-primary px-4" disabled={saving}>
                      {saving ? '保存中…' : '保存'}
                    </button>
                  </div>
                </form>
              </div>
            </div>
          )}
        </DividedStack>
      </SegmentedFrame>
    </div>
  );
}
