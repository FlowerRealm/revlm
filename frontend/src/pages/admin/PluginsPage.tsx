import { useCallback, useEffect, useRef, useState } from 'react';

import { SegmentedFrame } from '../../components/SegmentedFrame';
import {
  listPlugins,
  setPluginEnabled,
  uninstallPlugin,
  uploadPlugin,
  type PluginInstallation,
} from '../../api/plugins';

function statusBadge(status: string) {
  if (status === 'active') return 'bg-success bg-opacity-10 text-success border border-success-subtle';
  if (status === 'failed') return 'bg-danger bg-opacity-10 text-danger border border-danger-subtle';
  if (status === 'disabled') return 'bg-secondary bg-opacity-10 text-secondary border';
  return 'bg-warning bg-opacity-10 text-warning-emphasis border border-warning-subtle';
}

export function PluginsPage() {
  const fileInput = useRef<HTMLInputElement | null>(null);
  const [plugins, setPlugins] = useState<PluginInstallation[]>([]);
  const [loading, setLoading] = useState(true);
  const [busy, setBusy] = useState('');
  const [notice, setNotice] = useState('');
  const [error, setError] = useState('');

  const reload = useCallback(async () => {
    setLoading(true);
    try {
      const res = await listPlugins();
      if (!res.success) throw new Error(res.message || '加载插件失败');
      setPlugins(res.data || []);
    } catch (e) {
      setError(e instanceof Error ? e.message : '加载插件失败');
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void reload();
  }, [reload]);

  async function runAction(key: string, action: () => Promise<{ success: boolean; message?: string }>) {
    setBusy(key);
    setError('');
    setNotice('');
    try {
      const res = await action();
      if (!res.success) throw new Error(res.message || '操作失败');
      setNotice(res.message || '操作已安排，重启服务后生效');
      await reload();
    } catch (e) {
      setError(e instanceof Error ? e.message : '操作失败');
    } finally {
      setBusy('');
    }
  }

  return (
    <div className="fade-in-up">
      <SegmentedFrame>
        <div className="d-flex justify-content-between align-items-start flex-wrap gap-3">
          <div>
            <h2 className="h4 fw-bold mb-1">插件管理</h2>
            <p className="text-muted small mb-0">
              上传已编译的 <code>.revlm-plugin</code> ZIP 包。重启时，已启用模块会在 worker
              启动前预加载，可直接覆盖核心中的普通 C++ 函数；当前实例不会热加载。
            </p>
          </div>
          <div>
            <input
              ref={fileInput}
              className="d-none"
              type="file"
              accept=".revlm-plugin,application/zip"
              onChange={(event) => {
                const file = event.target.files?.[0];
                if (!file) return;
                void runAction(`upload:${file.name}`, () => uploadPlugin(file));
                event.currentTarget.value = '';
              }}
            />
            <button
              type="button"
              className="btn btn-primary"
              disabled={busy !== ''}
              onClick={() => fileInput.current?.click()}
            >
              <i className="ri-upload-2-line me-1"></i>
              {busy.startsWith('upload:') ? '上传中…' : '上传插件'}
            </button>
          </div>
        </div>

        <div className="alert alert-warning mt-3 mb-0 small" role="alert">
          安装插件表示无条件信任其前后端代码。插件不受 SDK、路由表或能力白名单限制；卸载不会执行 down
          migration，也不会删除历史数据。
        </div>
        {notice ? <div className="alert alert-success mt-3 mb-0">{notice}</div> : null}
        {error ? <div className="alert alert-danger mt-3 mb-0">{error}</div> : null}

        <div className="card border-0 shadow-sm overflow-hidden mt-3">
          <div className="table-responsive">
            <table className="table table-hover align-middle mb-0">
              <thead className="table-light">
                <tr>
                  <th className="ps-4">插件</th>
                  <th>状态</th>
                  <th>目标</th>
                  <th>迁移</th>
                  <th className="text-end pe-4">操作</th>
                </tr>
              </thead>
              <tbody>
                {loading ? (
                  <tr>
                    <td colSpan={5} className="text-center py-5 text-muted">
                      加载中…
                    </td>
                  </tr>
                ) : plugins.length === 0 ? (
                  <tr>
                    <td colSpan={5} className="text-center py-5 text-muted">
                      尚未安装插件。
                    </td>
                  </tr>
                ) : (
                  plugins.map((plugin) => {
                    const actionKey = `plugin:${plugin.id}`;
                    const isBusy = busy === actionKey;
                    return (
                      <tr key={plugin.id}>
                        <td className="ps-4">
                          <div className="fw-semibold">{plugin.name || plugin.id}</div>
                          <div className="text-muted small font-monospace">
                            {plugin.id} · v{plugin.version}
                          </div>
                          <div className="text-muted small">{plugin.core_abi}</div>
                          {plugin.error ? <div className="text-danger small mt-1">{plugin.error}</div> : null}
                        </td>
                        <td>
                          <span className={`badge ${statusBadge(plugin.status)}`}>{plugin.status}</span>
                          {!plugin.enabled ? <div className="text-muted small mt-1">已请求停用</div> : null}
                          {plugin.system_plugin ? <div className="text-muted small mt-1">系统插件</div> : null}
                        </td>
                        <td className="font-monospace small">
                          {plugin.target.os}/{plugin.target.arch}
                        </td>
                        <td className="small">
                          {plugin.migrations.length === 0
                            ? '-'
                            : plugin.migrations.map((migration) => (
                                <div key={migration.id} className="font-monospace text-muted">
                                  {migration.id}
                                </div>
                              ))}
                        </td>
                        <td className="text-end pe-4 text-nowrap">
                          <button
                            type="button"
                            className={`btn btn-sm ${plugin.enabled ? 'btn-light border text-warning' : 'btn-light border text-success'}`}
                            disabled={busy !== ''}
                            onClick={() => {
                              void runAction(actionKey, () => setPluginEnabled(plugin.id, !plugin.enabled));
                            }}
                          >
                            {isBusy ? '处理中…' : plugin.enabled ? '停用' : '启用'}
                          </button>
                          {!plugin.system_plugin ? (
                            <button
                              type="button"
                              className="btn btn-sm btn-light border text-danger ms-2"
                              disabled={busy !== ''}
                              onClick={() => {
                                if (!window.confirm(`确认卸载插件 ${plugin.name || plugin.id}？历史数据会保留。`))
                                  return;
                                void runAction(actionKey, () => uninstallPlugin(plugin.id));
                              }}
                            >
                              卸载
                            </button>
                          ) : null}
                        </td>
                      </tr>
                    );
                  })
                )}
              </tbody>
            </table>
          </div>
        </div>
      </SegmentedFrame>
    </div>
  );
}
