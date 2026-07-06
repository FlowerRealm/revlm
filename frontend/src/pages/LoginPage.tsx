import { useMemo, useState } from 'react';
import { Link, Navigate, useLocation, useNavigate } from 'react-router-dom';

import { useAuth } from '../auth/AuthContext';
import { SegmentedFrame } from '../components/SegmentedFrame';
import { formatAuthError, type PageError } from '../format/authError';

type LocationState = {
  from?: string;
  notice?: string;
  error?: string;
};

export function LoginPage() {
  const { user, login, loading } = useAuth();
  const navigate = useNavigate();
  const location = useLocation();

  const [form, setForm] = useState({ login: '', password: '' });
  const [err, setErr] = useState<PageError | null>(null);

  const notice = useMemo(() => {
    const state = location.state as LocationState | null;
    const v = (state?.notice || '').toString().trim();
    return v ? v : '';
  }, [location.state]);

  const routedError = useMemo<PageError | null>(() => {
    const state = location.state as LocationState | null;
    const v = (state?.error || '').toString().trim();
    return v ? formatAuthError('登录', v) : null;
  }, [location.state]);

  const from = useMemo(() => {
    const state = location.state as LocationState | null;
    const next = (state?.from || '').toString().trim();
    return next || '/dashboard';
  }, [location.state]);

  const effectiveError = err || routedError;

  if (user) {
    return <Navigate to="/dashboard" replace />;
  }

  return (
    <SegmentedFrame>
      <div className="card border-0 mb-0">
        <div className="card-body p-4">
          <h2 className="h4 card-title text-center mb-4">登录 Revlm</h2>

          {notice ? (
            <div className="alert alert-success py-2" role="alert">
              <span className="me-1 material-symbols-rounded">check_circle</span> {notice}
            </div>
          ) : null}

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
                await login(form.login, form.password);
                navigate(from, { replace: true });
              } catch (e) {
                setErr(formatAuthError('登录', e));
              }
            }}
          >
            <div className="mb-3">
              <label className="form-label">邮箱或账号名</label>
              <input
                className="form-control"
                name="login"
                type="text"
                autoComplete="username"
                required
                placeholder="name@example.com 或 alice"
                value={form.login}
                onChange={(e) => setForm((p) => ({ ...p, login: e.target.value }))}
              />
            </div>

            <div className="mb-3">
              <label className="form-label">密码</label>
              <input
                className="form-control"
                name="password"
                type="password"
                autoComplete="current-password"
                required
                placeholder="******"
                value={form.password}
                onChange={(e) => setForm((p) => ({ ...p, password: e.target.value }))}
              />
            </div>

            <div className="d-grid mt-4">
              <button type="submit" className="btn btn-primary" disabled={loading}>
                {loading ? '登录中…' : '立即登录'}
              </button>
            </div>
          </form>
        </div>

        <div className="card-footer bg-transparent text-center py-3">
          <span className="text-muted small">还没有账号？</span>{' '}
          <Link to="/register" className="text-decoration-none">
            注册新账号
          </Link>
        </div>
      </div>
    </SegmentedFrame>
  );
}
