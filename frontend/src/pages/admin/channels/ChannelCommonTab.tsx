import { useEffect, useState } from 'react';

import { createChannel, updateChannel, type Channel } from '../../../api/channels';
import { type AdminChannelGroup } from '../../../api/admin/channelGroups';

import { parseGroupsCSV, toggleGroupsCSV } from './utils';

type ChannelPatch = Partial<
  Pick<
    Channel,
    'type' | 'name' | 'status' | 'base_url' | 'groups' | 'priority' | 'api_key' | 'price_multiplier' | 'config_json'
  >
>;

type ChannelCommonTabProps = {
  mode?: 'create' | 'edit';
  enabled: boolean;
  channelID?: number;
  channelGroups: AdminChannelGroup[];
  editType: string;
  setEditType: (value: string) => void;
  editName: string;
  setEditName: (value: string) => void;
  editStatus: boolean;
  setEditStatus: (value: boolean) => void;
  editBaseURL: string;
  setEditBaseURL: (value: string) => void;
  editKey: string;
  setEditKey: (value: string) => void;
  editGroups: string;
  setEditGroups: (value: string) => void;
  editPriority: string;
  setEditPriority: (value: string) => void;
  editPriceMultiplier: string;
  setEditPriceMultiplier: (value: string) => void;
  editConfigJSON: Record<string, unknown>;
  setEditConfigJSON: (value: Record<string, unknown>) => void;
  applyChannelPatch?: (id: number, patch: ChannelPatch) => void;
  onCreated?: (id: number) => void | Promise<void>;
};

export function ChannelCommonTab({
  mode = 'edit',
  enabled,
  channelID,
  channelGroups,
  editType,
  setEditType,
  editName,
  setEditName,
  editStatus,
  setEditStatus,
  editBaseURL,
  setEditBaseURL,
  editKey,
  setEditKey,
  editGroups,
  setEditGroups,
  editPriority,
  setEditPriority,
  editPriceMultiplier,
  setEditPriceMultiplier,
  editConfigJSON,
  setEditConfigJSON,
  applyChannelPatch,
  onCreated,
}: ChannelCommonTabProps) {
  const [saving, setSaving] = useState(false);
  const [err, setErr] = useState('');
  const [notice, setNotice] = useState('');
  const [visibleKey, setVisibleKey] = useState(false);
  const [copied, setCopied] = useState(false);
  const [configText, setConfigText] = useState('{}');

  useEffect(() => {
    setSaving(false);
    setErr('');
    setNotice('');
    setVisibleKey(false);
    setCopied(false);
    setConfigText(JSON.stringify(editConfigJSON || {}, null, 2));
  }, [channelID, editConfigJSON, mode]); // Switching the target is intentionally a fresh editor.

  async function copyKey() {
    if (!editKey) return;
    await navigator.clipboard.writeText(editKey);
    setCopied(true);
    window.setTimeout(() => setCopied(false), 1500);
  }

  async function saveCommonSettings() {
    const type = editType.trim();
    const name = editName.trim();
    const baseURL = editBaseURL.trim();
    if (!type) {
      setErr('渠道类型不能为空');
      return;
    }
    if (!name) {
      setErr('名称不能为空');
      return;
    }
    if (!baseURL) {
      setErr('接口基础地址不能为空');
      return;
    }
    const priceMultiplier = Number.parseFloat(editPriceMultiplier);
    if (!Number.isFinite(priceMultiplier) || priceMultiplier < 0) {
      setErr('价格倍率必须是非负数字');
      return;
    }
    let configJSON: Record<string, unknown>;
    try {
      const parsed: unknown = JSON.parse(configText || '{}');
      if (!parsed || Array.isArray(parsed) || typeof parsed !== 'object') throw new Error();
      configJSON = parsed as Record<string, unknown>;
    } catch {
      setErr('扩展配置必须是 JSON 对象');
      return;
    }

    setSaving(true);
    setErr('');
    setNotice('');
    try {
      if (mode === 'create') {
        const res = await createChannel({
          type,
          name,
          status: editStatus,
          base_url: baseURL,
          key: editKey,
          groups: editGroups.trim(),
          priority: Number.parseInt(editPriority, 10) || 0,
          price_multiplier: priceMultiplier,
          config_json: configJSON,
        });
        if (!res.success || !res.data?.id) throw new Error(res.message || '创建失败');
        setEditConfigJSON(configJSON);
        await onCreated?.(res.data.id);
        setNotice('已创建');
        return;
      }
      if (!channelID) throw new Error('渠道不存在');
      const res = await updateChannel({
        id: channelID,
        type,
        name,
        status: editStatus,
        base_url: baseURL,
        key: editKey,
        groups: editGroups.trim(),
        priority: Number.parseInt(editPriority, 10) || 0,
        price_multiplier: priceMultiplier,
        config_json: configJSON,
      });
      if (!res.success) throw new Error(res.message || '保存失败');
      setEditConfigJSON(configJSON);
      applyChannelPatch?.(channelID, {
        type,
        name,
        status: editStatus,
        base_url: baseURL,
        api_key: editKey,
        groups: editGroups.trim(),
        priority: Number.parseInt(editPriority, 10) || 0,
        price_multiplier: priceMultiplier,
        config_json: configJSON,
      });
      setNotice('已保存');
    } catch (error) {
      setErr(error instanceof Error ? error.message : '保存失败');
    } finally {
      setSaving(false);
    }
  }

  const formKey = channelID ?? 'create';
  const disabled = saving || !enabled;

  return (
    <div className="d-flex flex-column gap-3">
      <div className="card border-0 shadow-sm">
        <div className="card-header bg-white fw-bold py-3">渠道设置</div>
        <div className="card-body">
          <form
            className="row g-3"
            onSubmit={(event) => {
              event.preventDefault();
              void saveCommonSettings();
            }}
          >
            {notice ? (
              <div className="col-12">
                <div className="alert alert-success py-2 mb-0">{notice}</div>
              </div>
            ) : null}
            {err ? (
              <div className="col-12">
                <div className="alert alert-danger py-2 mb-0">{err}</div>
              </div>
            ) : null}
            <div className="col-md-5">
              <label className="form-label fw-medium">渠道类型</label>
              <input
                className="form-control font-monospace"
                value={editType}
                onChange={(event) => setEditType(event.target.value)}
                placeholder="例如 my_provider"
                disabled={disabled}
                required
              />
              <div className="form-text small text-muted">核心不验证类型；对应插件自己解释它。</div>
            </div>
            <div className="col-md-4">
              <label className="form-label fw-medium">名称</label>
              <input
                className="form-control"
                value={editName}
                onChange={(event) => setEditName(event.target.value)}
                disabled={disabled}
                required
              />
            </div>
            <div className="col-md-3">
              <label className="form-label fw-medium">状态</label>
              <select
                className="form-select"
                value={editStatus ? 'true' : 'false'}
                onChange={(event) => setEditStatus(event.target.value === 'true')}
                disabled={disabled}
              >
                <option value="true">启用</option>
                <option value="false">禁用</option>
              </select>
            </div>
            <div className="col-12">
              <label className="form-label fw-medium">接口基础地址</label>
              <input
                className="form-control font-monospace"
                value={editBaseURL}
                onChange={(event) => setEditBaseURL(event.target.value)}
                disabled={disabled}
                required
              />
            </div>
            <div className="col-12">
              <label className="form-label fw-medium">API 密钥</label>
              <div className="d-flex flex-column gap-2">
                <input
                  className="form-control font-monospace"
                  type={visibleKey ? 'text' : 'password'}
                  value={editKey}
                  onChange={(event) => setEditKey(event.target.value)}
                  disabled={disabled}
                  placeholder="sk-..."
                  autoComplete="new-password"
                />
                <div className="d-flex gap-2">
                  <button
                    type="button"
                    className="btn btn-sm btn-light border"
                    disabled={disabled}
                    onClick={() => setVisibleKey((value) => !value)}
                  >
                    {visibleKey ? '隐藏' : '查看'}
                  </button>
                  <button
                    type="button"
                    className="btn btn-sm btn-light border"
                    disabled={disabled || !editKey}
                    onClick={() => {
                      void copyKey().catch(() => {});
                    }}
                  >
                    {copied ? '已复制' : '复制'}
                  </button>
                </div>
              </div>
              <div className="form-text small text-muted">密钥以明文存储；留空表示清除。</div>
            </div>
            <div className="col-12">
              <label className="form-label fw-medium">渠道组设置</label>
              <div className="card p-2" style={{ maxHeight: 260, overflowY: 'auto' }}>
                {channelGroups.length === 0 ? (
                  <div className="text-muted small px-2 py-1">暂无渠道组（请先到“渠道组”创建）。</div>
                ) : (
                  channelGroups.map((group) => {
                    const selected = parseGroupsCSV(editGroups).includes(group.name);
                    const groupDisabled = !group.status && !selected;
                    return (
                      <div className="form-check" key={group.id}>
                        <input
                          className="form-check-input"
                          type="checkbox"
                          id={`group_edit_${formKey}_${group.name}`}
                          checked={selected}
                          disabled={groupDisabled || disabled}
                          onChange={(event) =>
                            setEditGroups(toggleGroupsCSV(editGroups, group.name, event.target.checked))
                          }
                        />
                        <label className="form-check-label w-100" htmlFor={`group_edit_${formKey}_${group.name}`}>
                          {group.name}{' '}
                          {!group.status ? <span className="badge bg-secondary ms-1 smaller">禁用</span> : null}
                        </label>
                      </div>
                    );
                  })
                )}
              </div>
            </div>
            <div className="col-md-6">
              <label className="form-label fw-medium">优先级</label>
              <input
                className="form-control"
                value={editPriority}
                onChange={(event) => setEditPriority(event.target.value)}
                inputMode="numeric"
                disabled={disabled}
              />
            </div>
            <div className="col-md-6">
              <label className="form-label fw-medium">价格倍率</label>
              <input
                className="form-control"
                value={editPriceMultiplier}
                onChange={(event) => setEditPriceMultiplier(event.target.value)}
                inputMode="decimal"
                disabled={disabled}
              />
            </div>
            <div className="col-12">
              <label className="form-label fw-medium">扩展配置 JSON</label>
              <textarea
                className="form-control font-monospace"
                rows={7}
                value={configText}
                onChange={(event) => setConfigText(event.target.value)}
                disabled={disabled}
                spellCheck={false}
              />
              <div className="form-text small text-muted">这是原样交给插件的配置。插件前端也可以完全替换这张表单。</div>
            </div>
            <div className="col-12 d-flex justify-content-end">
              <button type="submit" className="btn btn-primary px-4" disabled={disabled}>
                {saving ? (mode === 'create' ? '创建中…' : '保存中…') : mode === 'create' ? '创建渠道' : '保存'}
              </button>
            </div>
          </form>
        </div>
      </div>
    </div>
  );
}
