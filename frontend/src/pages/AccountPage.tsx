import { useMemo, useState } from 'react';
import { useNavigate } from 'react-router-dom';

import { useAuth } from '../auth/AuthContext';
import { DividedStack } from '../components/DividedStack';
import { SegmentedFrame } from '../components/SegmentedFrame';
import { ConfigForm } from '../components/admin/ConfigForm';
import { accountEmailTemplate, accountPasswordTemplate } from '../components/admin/configTemplates';

export function AccountPage() {
  const { user, logout, refresh } = useAuth();
  const navigate = useNavigate();

  const [err, setErr] = useState('');
  const [notice, setNotice] = useState('');

  const displayEmail = useMemo(() => (user?.email || '').toString(), [user?.email]);

  async function forceLogout(msg: string) {
    try {
      await logout();
    } catch {
      // ignore
    }
    try {
      await refresh();
    } catch {
      // ignore
    }
    navigate('/login', { replace: true, state: { notice: msg || '请重新登录' } });
  }

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
                  <span className="fs-4 material-symbols-rounded">manage_accounts</span>
                </div>
                <div>
                  <h5 className="mb-1 fw-semibold">账号设置</h5>
                  <p className="mb-0 text-muted small">修改邮箱/密码成功后需要重新登录。</p>
                </div>
              </div>
            </div>
          </div>

          {notice ? (
            <div className="alert alert-success py-2 mb-0" role="alert">
              <span className="me-1 material-symbols-rounded">check_circle</span> {notice}
            </div>
          ) : null}

          {err ? (
            <div className="alert alert-danger py-2 mb-0" role="alert">
              <span className="me-1 material-symbols-rounded">warning</span> {err}
            </div>
          ) : null}

          <div className="row g-4 mb-0">
            <div className="col-lg-6">
              <div className="card h-100">
                <div className="card-body">
                  <h5 className="fw-semibold mb-3">
                    <span className="me-2 text-primary material-symbols-rounded">alternate_email</span>账号名
                  </h5>
                  <div className="mb-3">
                    <label className="form-label">账号名</label>
                    <input
                      name="username"
                      type="text"
                      className="form-control"
                      autoComplete="username"
                      disabled
                      value={(user?.username || '').toString()}
                    />
                    <div className="form-text">账号名不可修改；用于登录（区分大小写，仅字母/数字）。</div>
                  </div>
                </div>
              </div>
            </div>

            <div className="col-lg-6">
              <div className="card h-100">
                <div className="card-body">
                  <h5 className="fw-semibold mb-3">
                    <span className="me-2 text-primary material-symbols-rounded">mail</span>邮箱
                  </h5>
                  <div className="mb-2 small text-muted">
                    当前邮箱：<strong className="text-dark">{displayEmail || '-'}</strong>
                  </div>

                  <ConfigForm
                    template={accountEmailTemplate}
                    layout="stack"
                    submitClassName="btn btn-primary"
                    onSubmitStart={() => {
                      setErr('');
                      setNotice('');
                    }}
                    onError={(message) => setErr(message || '保存失败')}
                    onResult={async (result) => {
                      if (result?.force_logout) {
                        await forceLogout('邮箱已更新，请重新登录');
                        return;
                      }
                      setNotice('已保存');
                    }}
                  />
                </div>
              </div>
            </div>

            <div className="col-lg-6">
              <div className="card h-100">
                <div className="card-body">
                  <h5 className="fw-semibold mb-3">
                    <span className="me-2 text-primary material-symbols-rounded">key</span>修改密码
                  </h5>
                  <ConfigForm
                    template={accountPasswordTemplate}
                    layout="stack"
                    submitClassName="btn btn-primary"
                    onSubmitStart={() => {
                      setErr('');
                      setNotice('');
                    }}
                    onError={(message) => setErr(message || '保存失败')}
                    onResult={async (result) => {
                      if (result?.force_logout) {
                        await forceLogout('密码已更新，请重新登录');
                        return;
                      }
                      setNotice('已保存');
                    }}
                  />
                </div>
              </div>
            </div>
          </div>
        </DividedStack>
      </SegmentedFrame>
    </div>
  );
}
