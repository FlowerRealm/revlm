import { useEffect, useMemo, useState } from 'react';

import {
  addAdminChannelGroupChannelMember,
  createAdminChannelGroup,
  deleteAdminChannelGroup,
  deleteAdminChannelGroupChannelMember,
  getAdminChannelGroupDetail,
  getAdminChannelGroupPointer,
  listAdminChannelGroups,
  reorderAdminChannelGroupMembers,
  setAdminDefaultChannelGroup,
  updateAdminChannelGroup,
  upsertAdminChannelGroupPointer,
  type AdminChannelGroup,
  type AdminChannelGroupDetail,
  type AdminChannelGroupMember,
  type AdminChannelGroupPointer,
} from '../../api/admin/channelGroups';
import { BootstrapModal } from '../../components/BootstrapModal';
import { DividedStack } from '../../components/DividedStack';
import { SegmentedFrame } from '../../components/SegmentedFrame';
import { AdminConfigSection } from '../../components/admin/AdminConfigWorkspace';
import { closeModalById, showModalById } from '../../components/modal';
import { useAdminSelectionParam } from '../../hooks/useAdminSelectionParam';

const MODAL_ID = 'channelGroupModal';

function statusBadge(status: number): { cls: string; label: string } {
  if (status === 1) return { cls: 'badge rounded-pill bg-success bg-opacity-10 text-success px-2', label: '启用' };
  return { cls: 'badge rounded-pill bg-secondary bg-opacity-10 text-secondary px-2', label: '禁用' };
}

function channelTypeLabel(type: string): string {
  if (type === 'openai_compatible') return 'OpenAI 兼容';
  if (type === 'anthropic') return 'Anthropic';
  return type;
}

function memberType(member: AdminChannelGroupMember): 'channel' | 'unknown' {
  if (member.member_channel_id) return 'channel';
  return 'unknown';
}

type ChannelGroupDraft = {
  name: string;
  description: string;
  price_multiplier: number;
  status: number;
};

function emptyDraft(): ChannelGroupDraft {
  return {
    name: '',
    description: '',
    price_multiplier: 1,
    status: 0,
  };
}

function groupToDraft(group: AdminChannelGroup): ChannelGroupDraft {
  return {
    name: group.name || '',
    description: group.description || '',
    price_multiplier: group.price_multiplier || 1,
    status: group.status || 0,
  };
}

function serializeDraft(value: {
  name: string;
  description?: string | null;
  price_multiplier?: number;
  status: number;
}) {
  return JSON.stringify(value);
}

export function ChannelGroupsPage() {
  const [groups, setGroups] = useState<AdminChannelGroup[]>([]);
  const [detail, setDetail] = useState<AdminChannelGroupDetail | null>(null);
  const [pointer, setPointer] = useState<AdminChannelGroupPointer | null>(null);
  const [loading, setLoading] = useState(true);
  const [detailLoading, setDetailLoading] = useState(false);
  const [creatingDraft, setCreatingDraft] = useState(false);
  const [saving, setSaving] = useState(false);
  const [err, setErr] = useState('');
  const [notice, setNotice] = useState('');
  const [selectedParam, setSelectedParam] = useAdminSelectionParam('group');

  const [selectedGroup, setSelectedGroup] = useState<AdminChannelGroup | null>(null);
  const [draft, setDraft] = useState<ChannelGroupDraft>(emptyDraft());
  const [savedSnapshot, setSavedSnapshot] = useState('');
  const [addChannelID, setAddChannelID] = useState('');

  const enabledCount = useMemo(() => groups.filter((group) => group.status === 1).length, [groups]);
  const selectedID = useMemo(() => {
    const parsed = Number.parseInt(selectedParam, 10);
    return Number.isFinite(parsed) && parsed > 0 ? parsed : 0;
  }, [selectedParam]);
  const members = detail?.members || [];
  const availableChannels = detail?.channels || [];

  const normalizedValue = useMemo(
    () => ({
      name: draft.name.trim(),
      description: draft.description.trim() || null,
      price_multiplier: draft.price_multiplier > 0 ? draft.price_multiplier : undefined,
      status: draft.status,
    }),
    [draft]
  );
  const isDirty = !!selectedGroup && serializeDraft(normalizedValue) !== savedSnapshot;
  const saveBlockedReason = useMemo(() => {
    if (!selectedGroup) return '未选择渠道组';
    if (!normalizedValue.name) return '名称不能为空';
    if (!isDirty) return '没有未保存改动';
    return '';
  }, [isDirty, normalizedValue, selectedGroup]);

  async function refresh() {
    setErr('');
    setLoading(true);
    try {
      const res = await listAdminChannelGroups();
      if (!res.success) throw new Error(res.message || '加载失败');
      setGroups(res.data || []);
    } catch (e) {
      setErr(e instanceof Error ? e.message : '加载失败');
      setGroups([]);
    } finally {
      setLoading(false);
    }
  }

  async function refreshDetail(groupID: number, opts?: { syncForm?: boolean }) {
    setDetailLoading(true);
    try {
      const [detailRes, pointerRes] = await Promise.all([
        getAdminChannelGroupDetail(groupID),
        getAdminChannelGroupPointer(groupID),
      ]);
      if (!detailRes.success) throw new Error(detailRes.message || '加载详情失败');
      const nextDetail = detailRes.data || null;
      const group = nextDetail?.group || null;
      setSelectedGroup(group);
      if (opts?.syncForm !== false) {
        const nextDraft = group ? groupToDraft(group) : emptyDraft();
        setDraft(nextDraft);
        setSavedSnapshot(
          serializeDraft({
            name: nextDraft.name.trim(),
            description: nextDraft.description.trim() || null,
            price_multiplier: nextDraft.price_multiplier > 0 ? nextDraft.price_multiplier : undefined,
            status: nextDraft.status,
          })
        );
        setAddChannelID('');
      }
      setDetail(nextDetail);
      setPointer(pointerRes.success ? pointerRes.data || null : null);
    } catch (e) {
      setErr(e instanceof Error ? e.message : '加载详情失败');
      setSelectedGroup(null);
      setDetail(null);
      setPointer(null);
      setDraft(emptyDraft());
      setSavedSnapshot('');
      if (opts?.syncForm !== false) {
        setAddChannelID('');
      }
    } finally {
      setDetailLoading(false);
    }
  }

  async function handleStartCreate() {
    setErr('');
    setNotice('');
    setCreatingDraft(true);
    try {
      const res = await createAdminChannelGroup({
        name: `channel-group-${Date.now().toString(36)}`,
        price_multiplier: 1,
        status: 0,
      });
      if (!res.success || !res.data?.id) throw new Error(res.message || '创建失败');
      setSelectedParam(res.data.id);
      await refresh();
    } catch (e) {
      setErr(e instanceof Error ? e.message : '创建失败');
    } finally {
      setCreatingDraft(false);
    }
  }

  async function handleSave() {
    if (!selectedGroup || saveBlockedReason) return;
    setErr('');
    setNotice('');
    setSaving(true);
    try {
      const res = await updateAdminChannelGroup(selectedGroup.id, normalizedValue);
      if (!res.success) throw new Error(res.message || '保存失败');
      setSavedSnapshot(serializeDraft(normalizedValue));
      setNotice(res.message || '已保存');
      await refresh();
      await refreshDetail(selectedGroup.id);
    } catch (e) {
      setErr(e instanceof Error ? e.message : '保存失败');
    } finally {
      setSaving(false);
    }
  }

  async function handleDelete() {
    if (!selectedGroup) return;
    if (!window.confirm(draft.name.trim() ? `确认删除渠道组 ${draft.name.trim()}？` : '确认删除该渠道组？')) return;
    setErr('');
    setNotice('');
    setSaving(true);
    try {
      const res = await deleteAdminChannelGroup(selectedGroup.id);
      if (!res.success) throw new Error(res.message || '删除失败');
      setNotice(res.message || '已删除');
      closeModalById(MODAL_ID);
      await refresh();
    } catch (e) {
      setErr(e instanceof Error ? e.message : '删除失败');
    } finally {
      setSaving(false);
    }
  }

  function handleModalHidden() {
    setSelectedParam(null);
    setSelectedGroup(null);
    setDetail(null);
    setPointer(null);
    setDraft(emptyDraft());
    setSavedSnapshot('');
    setAddChannelID('');
    setDetailLoading(false);
  }

  useEffect(() => {
    void refresh();
  }, []);

  useEffect(() => {
    if (!selectedParam) return;
    showModalById(MODAL_ID);
  }, [selectedParam]);

  useEffect(() => {
    if (!selectedID) {
      setSelectedGroup(null);
      setDetail(null);
      setPointer(null);
      setDraft(emptyDraft());
      setSavedSnapshot('');
      setAddChannelID('');
      return;
    }
    void refreshDetail(selectedID);
  }, [selectedID]);

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <DividedStack>
          <div className="card mb-0">
            <div className="card-body d-flex flex-column flex-md-row justify-content-between align-items-center">
              <div className="d-flex align-items-center mb-3 mb-md-0">
                <div
                  className="bg-warning bg-opacity-10 text-warning rounded-circle d-flex align-items-center justify-content-center me-3"
                  style={{ width: 48, height: 48 }}
                >
                  <span className="fs-4 material-symbols-rounded">hub</span>
                </div>
                <div>
                  <h5 className="mb-1 fw-semibold">渠道组</h5>
                  <p className="mb-0 text-muted small">
                    {enabledCount} 启用 / {groups.length} 总计
                  </p>
                </div>
              </div>

              <button
                type="button"
                className="btn btn-primary btn-sm"
                disabled={creatingDraft}
                onClick={() => void handleStartCreate()}
              >
                <span className="material-symbols-rounded me-1">add</span>
                {creatingDraft ? '创建中…' : '新建渠道组'}
              </button>
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
          ) : groups.length === 0 ? (
            <div className="text-center py-5 text-muted">
              <span className="fs-1 d-block mb-3 material-symbols-rounded">inbox</span>
              暂无渠道组。
            </div>
          ) : (
            <div className="card overflow-hidden mb-0">
              <div className="table-responsive">
                <table className="table table-hover align-middle mb-0">
                  <thead className="table-light">
                    <tr>
                      <th className="ps-4">渠道组</th>
                      <th>倍率</th>
                      <th>描述</th>
                      <th>指针</th>
                      <th>状态</th>
                      <th className="text-end pe-4">操作</th>
                    </tr>
                  </thead>
                  <tbody>
                    {groups.map((group) => {
                      const status = statusBadge(group.status);
                      const pointerLabel =
                        typeof group.pointer_channel_id === 'number' && group.pointer_channel_id > 0
                          ? `${group.pointer_channel_name?.trim() || `channel-${group.pointer_channel_id}`}${group.pointer_pinned ? ' · 已固定' : ''}`
                          : '无指针';
                      return (
                        <tr key={group.id}>
                          <td className="ps-4">
                            <div className="d-flex align-items-center gap-2 flex-wrap">
                              <span className="fw-semibold">{group.name}</span>
                              {group.is_default ? (
                                <span className="badge bg-primary bg-opacity-10 text-primary border">默认</span>
                              ) : null}
                            </div>
                          </td>
                          <td className="text-muted small">x{group.price_multiplier}</td>
                          <td className="text-muted small">{group.description || '无描述'}</td>
                          <td className="text-muted small">{pointerLabel}</td>
                          <td>
                            <span className={status.cls}>{status.label}</span>
                          </td>
                          <td className="text-end pe-4">
                            <button
                              type="button"
                              className="btn btn-sm btn-light border"
                              onClick={() => setSelectedParam(group.id)}
                            >
                              编辑
                            </button>
                          </td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              </div>
            </div>
          )}
        </DividedStack>
      </SegmentedFrame>

      <BootstrapModal
        id={MODAL_ID}
        title="渠道组"
        dialogClassName="modal-dialog-centered modal-xl modal-dialog-scrollable"
        bodyClassName="rlm-admin-config-inline-form"
        footer={
          <>
            <div className="me-auto small text-muted">
              {isDirty ? saveBlockedReason || '有未保存改动' : '关闭时未保存改动会直接丢弃。'}
            </div>
            <button type="button" className="btn btn-light" data-bs-dismiss="modal">
              关闭
            </button>
            <button
              type="button"
              className="btn btn-primary"
              disabled={!!saveBlockedReason || saving || detailLoading}
              onClick={() => void handleSave()}
            >
              {saving ? '保存中…' : '保存'}
            </button>
          </>
        }
        onHidden={() => {
          void handleModalHidden();
        }}
      >
        {detailLoading ? (
          <div className="text-muted">加载中…</div>
        ) : !selectedGroup ? (
          <div className="text-muted">未找到该渠道组。</div>
        ) : (
          <>
            <div className="d-flex justify-content-between align-items-start gap-3 flex-wrap mb-3">
              <div className="text-muted small">
                倍率 x{selectedGroup.price_multiplier} · 更新时间 {selectedGroup.updated_at}
              </div>
              <div className="d-flex gap-2 flex-wrap">
                <button
                  type="button"
                  className="btn btn-outline-warning btn-sm"
                  disabled={selectedGroup.is_default || draft.status !== 1 || saving}
                  onClick={async () => {
                    setErr('');
                    setNotice('');
                    setSaving(true);
                    try {
                      const res = await setAdminDefaultChannelGroup(selectedGroup.id);
                      if (!res.success) throw new Error(res.message || '设置失败');
                      setNotice(res.message || '已设置默认渠道组');
                      await refresh();
                      await refreshDetail(selectedGroup.id, { syncForm: false });
                    } catch (e) {
                      setErr(e instanceof Error ? e.message : '设置失败');
                    } finally {
                      setSaving(false);
                    }
                  }}
                >
                  {selectedGroup.is_default ? '默认渠道组' : '设为默认'}
                </button>
                <button
                  type="button"
                  className="btn btn-outline-danger btn-sm"
                  disabled={saving}
                  onClick={() => void handleDelete()}
                >
                  删除渠道组
                </button>
              </div>
            </div>

            <AdminConfigSection
              id="channel-group-basic"
              title="基本信息"
              description="名称、描述、倍率、默认组和当前指针。"
            >
              <div className="row g-3">
                <div className="col-md-6">
                  <label className="form-label">渠道组名称</label>
                  <input
                    className="form-control"
                    value={draft.name}
                    onChange={(e) => setDraft((prev) => ({ ...prev, name: e.target.value }))}
                  />
                </div>
                <div className="col-md-6">
                  <label className="form-label">状态</label>
                  <select
                    className="form-select"
                    value={draft.status}
                    onChange={(e) =>
                      setDraft((prev) => ({ ...prev, status: Number.parseInt(e.target.value, 10) || 0 }))
                    }
                  >
                    <option value={1}>启用</option>
                    <option value={0}>禁用</option>
                  </select>
                </div>
                <div className="col-md-6">
                  <label className="form-label">价格倍率</label>
                  <div className="input-group">
                    <span className="input-group-text">×</span>
                    <input
                      className="form-control"
                      type="number"
                      step="any"
                      min="0"
                      value={draft.price_multiplier}
                      onChange={(e) =>
                        setDraft((prev) => ({
                          ...prev,
                          price_multiplier: Number.parseFloat(e.target.value) || 0,
                        }))
                      }
                    />
                  </div>
                </div>
                <div className="col-md-6">
                  <label className="form-label">当前指针</label>
                  <input
                    className="form-control bg-light"
                    value={pointer?.channel_name || (pointer?.channel_id ? `channel-${pointer.channel_id}` : '未设置')}
                    disabled
                  />
                </div>
                <div className="col-12">
                  <label className="form-label">描述</label>
                  <input
                    className="form-control"
                    value={draft.description}
                    onChange={(e) => setDraft((prev) => ({ ...prev, description: e.target.value }))}
                    placeholder="可选"
                  />
                </div>
              </div>
            </AdminConfigSection>

            <AdminConfigSection
              id="channel-group-members"
              title="成员管理"
              description="渠道组只允许直接包含渠道，并维护顺序和指针。"
            >
              <div className="border rounded p-3 mb-4">
                <div className="fw-semibold mb-2">添加渠道</div>
                <select
                  className="form-select mb-2"
                  value={addChannelID}
                  onChange={(e) => setAddChannelID(e.target.value)}
                >
                  <option value="">选择一个渠道…</option>
                  {availableChannels.map((channel) => (
                    <option key={channel.id} value={String(channel.id)}>
                      {channel.name} · {channelTypeLabel(channel.type)}
                    </option>
                  ))}
                </select>
                <button
                  type="button"
                  className="btn btn-outline-primary btn-sm"
                  disabled={!addChannelID}
                  onClick={async () => {
                    const channelID = Number.parseInt(addChannelID, 10);
                    if (!Number.isFinite(channelID) || channelID <= 0) return;
                    setErr('');
                    setNotice('');
                    try {
                      const res = await addAdminChannelGroupChannelMember(selectedGroup.id, channelID);
                      if (!res.success) throw new Error(res.message || '添加失败');
                      setAddChannelID('');
                      setNotice('已添加渠道');
                      await refreshDetail(selectedGroup.id, { syncForm: false });
                    } catch (e) {
                      setErr(e instanceof Error ? e.message : '添加失败');
                    }
                  }}
                >
                  添加到当前组
                </button>
              </div>

              {detailLoading ? (
                <div className="text-muted small">加载成员中…</div>
              ) : members.length === 0 ? (
                <div className="text-muted small">暂无成员，请先添加渠道。</div>
              ) : (
                <div className="table-responsive">
                  <table className="table table-hover align-middle mb-0">
                    <thead className="table-light">
                      <tr>
                        <th>成员</th>
                        <th>类型</th>
                        <th>状态</th>
                        <th>顺序</th>
                        <th className="text-end">操作</th>
                      </tr>
                    </thead>
                    <tbody>
                      {members.map((member, index) => {
                        const type = memberType(member);
                        const typeLabel = type === 'channel' ? '渠道' : '未知';
                        const status =
                          type === 'channel' ? statusBadge(member.member_channel_status ?? 0) : statusBadge(0);
                        const isPointerChannel =
                          type === 'channel' && pointer?.pinned && pointer.channel_id === member.member_channel_id;
                        return (
                          <tr key={member.member_id}>
                            <td>
                              <div className="d-flex flex-column">
                                <div className="d-flex align-items-center gap-2 flex-wrap">
                                  <span className="fw-semibold">
                                    {type === 'channel'
                                      ? member.member_channel_name || `channel-${member.member_channel_id}`
                                      : '-'}
                                  </span>
                                  {member.promotion ? (
                                    <span className="badge bg-warning bg-opacity-10 text-warning border">优先</span>
                                  ) : null}
                                  {isPointerChannel ? (
                                    <span className="badge bg-light text-warning border">指针</span>
                                  ) : null}
                                </div>
                                <div className="text-muted small">
                                  {type === 'channel'
                                    ? `${channelTypeLabel((member.member_channel_type || '').trim())} · channel ID：${member.member_channel_id || '-'}`
                                    : '未知成员'}
                                </div>
                              </div>
                            </td>
                            <td>{typeLabel}</td>
                            <td>
                              <span className={status.cls}>{status.label}</span>
                            </td>
                            <td>
                              <div className="d-flex gap-1">
                                <button
                                  type="button"
                                  className="btn btn-sm btn-light border"
                                  disabled={index === 0}
                                  onClick={async () => {
                                    const nextIDs = members.map((item) => item.member_id);
                                    [nextIDs[index - 1], nextIDs[index]] = [nextIDs[index], nextIDs[index - 1]];
                                    setErr('');
                                    setNotice('');
                                    try {
                                      const res = await reorderAdminChannelGroupMembers(selectedGroup.id, nextIDs);
                                      if (!res.success) throw new Error(res.message || '保存排序失败');
                                      setNotice('已更新顺序');
                                      await refreshDetail(selectedGroup.id, { syncForm: false });
                                    } catch (e) {
                                      setErr(e instanceof Error ? e.message : '保存排序失败');
                                    }
                                  }}
                                >
                                  上移
                                </button>
                                <button
                                  type="button"
                                  className="btn btn-sm btn-light border"
                                  disabled={index === members.length - 1}
                                  onClick={async () => {
                                    const nextIDs = members.map((item) => item.member_id);
                                    [nextIDs[index], nextIDs[index + 1]] = [nextIDs[index + 1], nextIDs[index]];
                                    setErr('');
                                    setNotice('');
                                    try {
                                      const res = await reorderAdminChannelGroupMembers(selectedGroup.id, nextIDs);
                                      if (!res.success) throw new Error(res.message || '保存排序失败');
                                      setNotice('已更新顺序');
                                      await refreshDetail(selectedGroup.id, { syncForm: false });
                                    } catch (e) {
                                      setErr(e instanceof Error ? e.message : '保存排序失败');
                                    }
                                  }}
                                >
                                  下移
                                </button>
                              </div>
                            </td>
                            <td className="text-end">
                              <div className="d-inline-flex gap-1 flex-wrap justify-content-end">
                                {type === 'channel' && member.member_channel_id ? (
                                  <button
                                    type="button"
                                    className="btn btn-sm btn-light border text-warning"
                                    disabled={isPointerChannel}
                                    onClick={async () => {
                                      if (!member.member_channel_id) return;
                                      if (!window.confirm('确认将该渠道设为该组指针？')) return;
                                      setErr('');
                                      setNotice('');
                                      try {
                                        const res = await upsertAdminChannelGroupPointer(selectedGroup.id, {
                                          channel_id: member.member_channel_id,
                                          pinned: true,
                                        });
                                        if (!res.success) throw new Error(res.message || '设置失败');
                                        setNotice('已设置指针');
                                        await refreshDetail(selectedGroup.id, { syncForm: false });
                                      } catch (e) {
                                        setErr(e instanceof Error ? e.message : '设置失败');
                                      }
                                    }}
                                  >
                                    设为指针
                                  </button>
                                ) : null}
                                <button
                                  type="button"
                                  className="btn btn-sm btn-light border text-danger"
                                  onClick={async () => {
                                    if (!window.confirm('确认从该组移除该成员？')) return;
                                    setErr('');
                                    setNotice('');
                                    try {
                                      if (type === 'channel' && member.member_channel_id) {
                                        const res = await deleteAdminChannelGroupChannelMember(
                                          selectedGroup.id,
                                          member.member_channel_id
                                        );
                                        if (!res.success) throw new Error(res.message || '移除失败');
                                      } else {
                                        throw new Error('成员类型不合法');
                                      }
                                      setNotice('已移除成员');
                                      await refreshDetail(selectedGroup.id, { syncForm: false });
                                    } catch (e) {
                                      setErr(e instanceof Error ? e.message : '移除失败');
                                    }
                                  }}
                                >
                                  移除
                                </button>
                              </div>
                            </td>
                          </tr>
                        );
                      })}
                    </tbody>
                  </table>
                </div>
              )}
            </AdminConfigSection>
          </>
        )}
      </BootstrapModal>
    </div>
  );
}
