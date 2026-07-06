import { useEffect, useMemo, useState } from 'react';

import { useAuth } from '../../auth/AuthContext';
import { deleteAdminUser, listAdminUsers, updateAdminUser, type AdminUser } from '../../api/admin/users';
import { BootstrapModal } from '../../components/BootstrapModal';
import { DividedStack } from '../../components/DividedStack';
import { SegmentedFrame } from '../../components/SegmentedFrame';
import { closeModalById } from '../../components/modal';
import { ConfigForm } from '../../components/admin/ConfigForm';
import {
  addAdminUserBalanceTemplate,
  createAdminUserTemplate,
  resetAdminUserPasswordTemplate,
} from '../../components/admin/configTemplates';

function roleBadge(role: string): string {
  if (role === 'root')
    return 'badge rounded-pill bg-primary bg-opacity-10 text-primary border border-primary border-opacity-25 px-2';
  return 'badge rounded-pill bg-light text-secondary border px-2';
}

function statusBadge(status: number): { cls: string; label: string } {
  if (status === 1) return { cls: 'badge rounded-pill bg-success bg-opacity-10 text-success px-2', label: '启用' };
  return { cls: 'badge rounded-pill bg-secondary bg-opacity-10 text-secondary px-2', label: '禁用' };
}

export function UsersPage() {
  const { user: self } = useAuth();
  const selfID = self?.id || 0;

  const [users, setUsers] = useState<AdminUser[]>([]);
  const [loading, setLoading] = useState(true);
  const [err, setErr] = useState('');
  const [notice, setNotice] = useState('');

  const [editing, setEditing] = useState<AdminUser | null>(null);
  const [editEmail, setEditEmail] = useState('');
  const [editRole, setEditRole] = useState<'user' | 'root'>('user');
  const [editStatus, setEditStatus] = useState(1);

  const enabledCount = useMemo(() => users.filter((u) => u.status === 1).length, [users]);

  async function refresh() {
    setErr('');
    setNotice('');
    setLoading(true);
    try {
      const usersRes = await listAdminUsers();
      if (!usersRes.success) throw new Error(usersRes.message || '加载用户失败');
      setUsers(usersRes.data || []);
    } catch (e) {
      setErr(e instanceof Error ? e.message : '加载失败');
      setUsers([]);
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => {
    void refresh();
  }, []);

  useEffect(() => {
    if (!editing) return;
    setEditEmail(editing.email || '');
    setEditRole((editing.role || 'user') as 'user' | 'root');
    setEditStatus(editing.status || 0);
  }, [editing]);

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
                  <span className="fs-4 material-symbols-rounded">group</span>
                </div>
                <div>
                  <h5 className="mb-1 fw-semibold">用户管理</h5>
                  <p className="mb-0 text-muted small">
                    {enabledCount} 启用 / {users.length} 总计 · 仅 root 可管理用户
                  </p>
                </div>
              </div>

              <div className="d-flex gap-2">
                <button
                  type="button"
                  className="btn btn-primary btn-sm"
                  data-bs-toggle="modal"
                  data-bs-target="#createUserModal"
                >
                  <span className="me-1 material-symbols-rounded">person_add</span> 创建用户
                </button>
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
          ) : users.length === 0 ? (
            <div className="text-center py-5 text-muted">
              <span className="fs-1 d-block mb-3 material-symbols-rounded">inbox</span>
              暂无用户。
            </div>
          ) : (
            <div className="card overflow-hidden mb-0">
              <div className="table-responsive">
                <table className="table table-hover align-middle mb-0">
                  <thead className="table-light">
                    <tr>
                      <th className="ps-4">邮箱</th>
                      <th>账号名</th>
                      <th>角色</th>
                      <th>状态</th>
                      <th>余额(USD)</th>
                      <th>创建时间</th>
                      <th className="text-end pe-4">操作</th>
                    </tr>
                  </thead>
                  <tbody>
                    {users.map((u) => {
                      const st = statusBadge(u.status);
                      return (
                        <tr key={u.id}>
                          <td className="ps-4">
                            <span className="fw-bold text-dark">{u.email}</span>
                          </td>
                          <td>
                            {u.username ? (
                              <span className="text-dark fw-medium user-select-all">{u.username}</span>
                            ) : (
                              <span className="text-muted small fst-italic">未设置</span>
                            )}
                          </td>
                          <td>
                            <span className={roleBadge(u.role)}>{u.role}</span>
                          </td>
                          <td>
                            <span className={st.cls}>{st.label}</span>
                          </td>
                          <td className="fw-medium text-dark">{u.balance_usd}</td>
                          <td className="text-muted small">{u.created_at}</td>
                          <td className="text-end pe-4 text-nowrap">
                            <div className="d-inline-flex gap-1">
                              <button
                                type="button"
                                className="btn btn-sm btn-light border text-success"
                                title="加余额"
                                data-bs-toggle="modal"
                                data-bs-target="#addBalanceModal"
                                onClick={() => setEditing(u)}
                              >
                                <i className="ri-money-dollar-circle-line"></i>
                              </button>
                              <button
                                type="button"
                                className="btn btn-sm btn-light border text-primary"
                                title="编辑用户"
                                data-bs-toggle="modal"
                                data-bs-target="#editUserModal"
                                onClick={() => setEditing(u)}
                              >
                                <i className="ri-edit-line"></i>
                              </button>
                              <button
                                type="button"
                                className="btn btn-sm btn-light border text-warning"
                                title="重置密码"
                                data-bs-toggle="modal"
                                data-bs-target="#resetPasswordModal"
                                onClick={() => setEditing(u)}
                              >
                                <i className="ri-key-2-line"></i>
                              </button>
                              <button
                                type="button"
                                className="btn btn-sm btn-light border text-danger"
                                title={u.id === selfID ? '不能删除当前登录用户' : '删除用户'}
                                disabled={u.id === selfID}
                                onClick={async () => {
                                  if (u.id === selfID) return;
                                  if (!window.confirm('确认删除该用户？此操作不可恢复。')) return;
                                  setErr('');
                                  setNotice('');
                                  try {
                                    const res = await deleteAdminUser(u.id);
                                    if (!res.success) throw new Error(res.message || '删除失败');
                                    setNotice('已删除');
                                    if (editing?.id === u.id) setEditing(null);
                                    await refresh();
                                  } catch (e) {
                                    setErr(e instanceof Error ? e.message : '删除失败');
                                  }
                                }}
                              >
                                <i className="ri-delete-bin-line"></i>
                              </button>
                            </div>
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

      <BootstrapModal id="createUserModal" title="创建用户" dialogClassName="modal-dialog-centered modal-lg">
        <ConfigForm
          template={createAdminUserTemplate}
          onSubmitStart={() => {
            setErr('');
            setNotice('');
          }}
          onError={(message) => setErr(message || '创建失败')}
          onSaved={async () => {
            setNotice('已创建');
            closeModalById('createUserModal');
            await refresh();
          }}
          resetOnSaved
          submitClassName="btn btn-primary px-4"
          footerStart={
            <button type="button" className="btn btn-light" data-bs-dismiss="modal">
              取消
            </button>
          }
        />
      </BootstrapModal>

      <BootstrapModal
        id="editUserModal"
        title={editing ? `编辑用户：${editing.email}` : '编辑用户'}
        dialogClassName="modal-dialog-centered modal-lg"
        onHidden={() => {
          setEditing(null);
        }}
      >
        {!editing ? (
          <div className="text-muted">未选择用户。</div>
        ) : (
          <form
            className="row g-3"
            onSubmit={async (e) => {
              e.preventDefault();
              if (!editing) return;
              setErr('');
              setNotice('');
              try {
                const res = await updateAdminUser(editing.id, {
                  email: editEmail.trim(),
                  role: editRole,
                  status: editStatus,
                });
                if (!res.success) throw new Error(res.message || '保存失败');
                setNotice('已保存');
                closeModalById('editUserModal');
                await refresh();
              } catch (e) {
                setErr(e instanceof Error ? e.message : '保存失败');
              }
            }}
          >
            <div className="col-md-6">
              <label className="form-label">邮箱</label>
              <input
                className="form-control"
                value={editEmail}
                onChange={(e) => setEditEmail(e.target.value)}
                required
              />
              <div className="form-text small text-muted">修改邮箱后，新邮箱会立即用于后续登录。</div>
            </div>
            <div className="col-md-6">
              <label className="form-label">账号名</label>
              <input className="form-control" value={editing.username || ''} disabled />
              <div className="form-text small text-muted">账号名不可修改；用于登录（区分大小写，仅字母/数字）。</div>
            </div>
            <div className="col-md-6">
              <label className="form-label">状态</label>
              <select
                className="form-select"
                value={editStatus}
                onChange={(e) => setEditStatus(Number.parseInt(e.target.value, 10) || 0)}
                disabled={editing.id === selfID}
              >
                <option value={1}>启用</option>
                <option value={0}>禁用</option>
              </select>
            </div>
            <div className="col-md-6">
              <label className="form-label">角色</label>
              <select
                className="form-select"
                value={editRole}
                onChange={(e) => setEditRole((e.target.value as 'user' | 'root') || 'user')}
                disabled={editing.id === selfID}
              >
                <option value="user">普通用户</option>
                <option value="root">超级管理员</option>
              </select>
              {editing.id === selfID ? (
                <div className="form-text small text-muted">不能修改当前登录用户的状态或角色。</div>
              ) : null}
            </div>
            <div className="modal-footer border-top-0 px-0 pb-0">
              <button type="button" className="btn btn-light" data-bs-dismiss="modal">
                取消
              </button>
              <button className="btn btn-primary px-4" type="submit">
                确认更改
              </button>
            </div>
          </form>
        )}
      </BootstrapModal>

      <BootstrapModal
        id="addBalanceModal"
        title={editing ? `加余额：${editing.email}` : '加余额'}
        dialogClassName="modal-dialog-centered"
        onHidden={() => {
          setEditing(null);
        }}
      >
        {!editing ? (
          <div className="text-muted">未选择用户。</div>
        ) : (
          <>
            <div className="alert alert-light border py-2 small">
              <div className="d-flex justify-content-between">
                <span className="text-muted">当前余额</span>
                <span className="fw-bold text-dark">{editing.balance_usd} USD</span>
              </div>
            </div>
            <ConfigForm
              template={addAdminUserBalanceTemplate(editing.id)}
              onSubmitStart={() => {
                setErr('');
                setNotice('');
              }}
              onError={(message) => setErr(message || '加余额失败')}
              onSaved={async () => {
                setNotice('已加余额');
                closeModalById('addBalanceModal');
                await refresh();
              }}
              submitClassName="btn btn-success px-4"
              footerStart={
                <button type="button" className="btn btn-light" data-bs-dismiss="modal">
                  取消
                </button>
              }
            />
          </>
        )}
      </BootstrapModal>

      <BootstrapModal
        id="resetPasswordModal"
        title={editing ? `重置密码：${editing.email}` : '重置密码'}
        dialogClassName="modal-dialog-centered"
        onHidden={() => {
          setEditing(null);
        }}
      >
        {!editing ? (
          <div className="text-muted">未选择用户。</div>
        ) : (
          <>
            <ConfigForm
              template={resetAdminUserPasswordTemplate(editing.id)}
              onSubmitStart={() => {
                setErr('');
                setNotice('');
              }}
              onError={(message) => setErr(message || '重置失败')}
              onSaved={async () => {
                setNotice('已重置密码');
                closeModalById('resetPasswordModal');
                await refresh();
              }}
              submitClassName="btn btn-primary px-4"
              footerStart={
                <button type="button" className="btn btn-light" data-bs-dismiss="modal">
                  取消
                </button>
              }
            />
          </>
        )}
      </BootstrapModal>
    </div>
  );
}
