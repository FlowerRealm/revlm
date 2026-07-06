import { useMemo, useState } from 'react';
import { Link, Navigate, useLocation, useNavigate } from 'react-router-dom';

import { useAuth } from '../auth/AuthContext';
import { SegmentedFrame } from '../components/SegmentedFrame';
import { formatAuthError, type PageError } from '../format/authError';

type LocationState = {
  notice?: string;
  error?: string;
};

export function RegisterPage() {
  const { user, register, loading } = useAuth();
  const navigate = useNavigate();
  const location = useLocation();

  const [form, setForm] = useState({
    email: '',
    username: '',
    password: '',
  });
  const [err, setErr] = useState<PageError | null>(null);

  const routedError = useMemo<PageError | null>(() => {
    const state = location.state as LocationState | null;
    const v = (state?.error || '').toString().trim();
    return v ? formatAuthError('注册', v) : null;
  }, [location.state]);

  const effectiveError = err || routedError;

  if (user) {
    return <Navigate to="/dashboard" replace />;
  }

  return (
    <SegmentedFrame>
      <div className="card border-0 mb-0">
        <div className="card-body p-4">
          <h2 className="h4 card-title text-center mb-4">注册账号</h2>

          {effectiveError ? (
            <div className="alert alert-danger py-2" role="alert">
              <span className="me-1 material-symbols-rounded">warning</span> {effectiveError.summary}
              {effectiveError.detail ? (
                <details className="mt-1">
                  <summary className="small">详情</summary>
                  <div className="small text-break">{effectiveError.detail}</div>
                </details>
              ) : null}
            </div>
          ) : null}

          <form
            onSubmit={async (e) => {
              e.preventDefault();
              setErr(null);
              try {
                await register(form.email.trim(), form.username.trim(), form.password);
                navigate('/dashboard', { replace: true });
              } catch (e) {
                setErr(formatAuthError('注册', e));
              }
            }}
          >
            <div className="mb-3">
              <label className="form-label">邮箱</label>
              <input
                name="email"
                type="email"
                className="form-control"
                autoComplete="email"
                required
                placeholder="name@example.com"
                value={form.email}
                onChange={(e) => setForm((p) => ({ ...p, email: e.target.value }))}
              />
            </div>

            <div className="mb-3">
              <label className="form-label">账号名</label>
              <input
                name="username"
                type="text"
                className="form-control"
                autoComplete="username"
                required
                placeholder="例如：alice"
                value={form.username}
                onChange={(e) => setForm((p) => ({ ...p, username: e.target.value }))}
              />
              <div className="form-text">仅允许字母/数字（区分大小写），最多 64 位；用于登录。</div>
            </div>

            <div className="mb-3">
              <label className="form-label">密码</label>
              <input
                name="password"
                type="password"
                className="form-control"
                autoComplete="new-password"
                required
                placeholder="至少 8 位字符"
                value={form.password}
                onChange={(e) => setForm((p) => ({ ...p, password: e.target.value }))}
              />
              <div className="form-text">密码将通过 bcrypt 加密存储。</div>
            </div>

            <div className="d-grid mt-4">
              <button type="submit" className="btn btn-primary" disabled={loading}>
                {loading ? '提交中…' : '创建账号'}
              </button>
            </div>
          </form>
        </div>

        <div className="card-footer bg-transparent text-center py-3">
          <span className="text-muted small">已有账号？</span>{' '}
          <Link to="/login" className="text-decoration-none">
            直接登录
          </Link>
        </div>
      </div>
    </SegmentedFrame>
  );
}
