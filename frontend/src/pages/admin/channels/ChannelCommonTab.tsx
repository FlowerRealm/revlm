import { useEffect, useState } from 'react';

import { updateChannel, type Channel } from '../../../api/channels';
import { type AdminChannelGroup } from '../../../api/admin/channelGroups';

import { parseGroupsCSV, toggleGroupsCSV } from './utils';

type ChannelPatch = Partial<Pick<Channel, 'name' | 'status' | 'base_url' | 'groups' | 'priority' | 'api_key'>>;

type ChannelCommonTabProps = {
  enabled: boolean;
  channelID: number;
  channelGroups: AdminChannelGroup[];
  editName: string;
  setEditName: (v: string) => void;
  editStatus: boolean;
  setEditStatus: (v: boolean) => void;
  editBaseURL: string;
  setEditBaseURL: (v: string) => void;
  editKey: string;
  setEditKey: (v: string) => void;
  editGroups: string;
  setEditGroups: (v: string) => void;
  editPriority: string;
  setEditPriority: (v: string) => void;
  applyChannelPatch: (id: number, patch: ChannelPatch) => void;
};

export function ChannelCommonTab({
  enabled,
  channelID,
  channelGroups,
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
  applyChannelPatch,
}: ChannelCommonTabProps) {
  const [saving, setSaving] = useState(false);
  const [err, setErr] = useState('');
  const [notice, setNotice] = useState('');
  const [visibleKey, setVisibleKey] = useState(false);
  const [copied, setCopied] = useState(false);

  useEffect(() => {
    setSaving(false);
    setErr('');
    setNotice('');
    setVisibleKey(false);
    setCopied(false);
  }, [channelID]);

  async function copyKey() {
    if (!editKey) {
      return;
    }
    await navigator.clipboard.writeText(editKey);
    setCopied(true);
    window.setTimeout(() => setCopied(false), 1500);
  }

  async function saveCommonSettings() {
    const value = {
      name: editName,
      status: editStatus,
      base_url: editBaseURL,
      key: editKey,
      groups: editGroups,
      priority: editPriority,
    };
    const name = value.name.trim();
    const baseURL = value.base_url.trim();
    if (!name) {
      setErr('名称不能为空');
      setNotice('');
      return;
    }
    if (!baseURL) {
      setErr('接口基础地址不能为空');
      setNotice('');
      return;
    }
    setSaving(true);
    setErr('');
    setNotice('');
    try {
      const res = await updateChannel({
        id: channelID,
        name,
        status: value.status,
        base_url: baseURL,
        key: value.key,
        groups: value.groups.trim(),
        priority: Number.parseInt(value.priority, 10) || 0,
      });
      if (!res.success) throw new Error(res.message || '保存失败');
      applyChannelPatch(channelID, {
        name,
        status: value.status,
        base_url: baseURL,
        api_key: value.key,
        groups: value.groups.trim(),
        priority: Number.parseInt(value.priority, 10) || 0,
      });
      setNotice('已保存');
    } catch (e) {
      setErr(e instanceof Error ? e.message : '保存失败');
    } finally {
      setSaving(false);
    }
  }

  return (
    <div className="d-flex flex-column gap-3">
      <div className="card border-0 shadow-sm">
        <div className="card-header bg-white fw-bold py-3">常用设置</div>
        <div className="card-body">
          <form
            className="row g-3"
            onSubmit={(e) => {
              e.preventDefault();
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
            <div className="col-md-8">
              <label className="form-label fw-medium">名称</label>
              <input
                className="form-control"
                value={editName}
                onChange={(e) => setEditName(e.target.value)}
                disabled={saving || !enabled}
                required
              />
            </div>
            <div className="col-md-4">
              <label className="form-label fw-medium">状态</label>
              <select
                className="form-select"
                value={editStatus ? 'true' : 'false'}
                onChange={(e) => setEditStatus(e.target.value === 'true')}
                disabled={saving || !enabled}
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
                onChange={(e) => setEditBaseURL(e.target.value)}
                disabled={saving || !enabled}
                required
              />
            </div>
            <div className="col-12">
              <label className="form-label fw-medium">API 密钥</label>
              <div className="d-flex flex-column gap-2">
                {visibleKey ? (
                  <input
                    className="form-control font-monospace"
                    value={editKey}
                    onChange={(e) => setEditKey(e.target.value)}
                    disabled={saving || !enabled}
                    placeholder="sk-..."
                    autoComplete="new-password"
                  />
                ) : (
                  <input
                    className="form-control font-monospace"
                    type="password"
                    value={editKey}
                    onChange={(e) => setEditKey(e.target.value)}
                    disabled={saving || !enabled}
                    placeholder="sk-..."
                    autoComplete="new-password"
                  />
                )}
                <div className="d-flex gap-2">
                  <button
                    type="button"
                    className="btn btn-sm btn-light border"
                    disabled={saving || !enabled}
                    onClick={() => setVisibleKey((v) => !v)}
                  >
                    {visibleKey ? '隐藏' : '查看'}
                  </button>
                  <button
                    type="button"
                    className="btn btn-sm btn-light border"
                    disabled={saving || !enabled || !editKey}
                    onClick={() => {
                      void copyKey().catch(() => { });
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
                  channelGroups.map((g) => {
                    const selected = parseGroupsCSV(editGroups).includes(g.name);
                    const disabled = g.status !== 1 && !selected;
                    return (
                      <div className="form-check" key={g.id}>
                        <input
                          className="form-check-input"
                          type="checkbox"
                          id={`group_edit_${channelID}_${g.name}`}
                          checked={selected}
                          disabled={disabled || saving || !enabled}
                          onChange={(e) => setEditGroups(toggleGroupsCSV(editGroups, g.name, e.target.checked))}
                        />
                        <label className="form-check-label w-100" htmlFor={`group_edit_${channelID}_${g.name}`}>
                          {g.name}{' '}
                          {g.status !== 1 ? <span className="badge bg-secondary ms-1 smaller">禁用</span> : null}
                        </label>
                      </div>
                    );
                  })
                )}
              </div>
              <div className="form-text small text-muted mt-2">用于上游调度选择渠道。</div>
            </div>

            <div className="col-md-6">
              <label className="form-label fw-medium">优先级</label>
              <input
                className="form-control"
                value={editPriority}
                onChange={(e) => setEditPriority(e.target.value)}
                inputMode="numeric"
                disabled={saving || !enabled}
              />
            </div>

            <div className="col-12 d-flex justify-content-end">
              <button type="submit" className="btn btn-primary px-4" disabled={saving || !enabled}>
                {saving ? '保存中…' : '保存'}
              </button>
            </div>
          </form>
        </div>
      </div>
    </div>
  );
}
