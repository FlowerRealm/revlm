import type { ReactNode } from 'react';
import { Navigate, useLocation } from 'react-router-dom';

import { useAuth } from './AuthContext';

type RequireAuthProps = {
  children: ReactNode;
  rootOnly?: boolean;
};

export function RequireAuth({ children, rootOnly = false }: RequireAuthProps) {
  const { user, booting } = useAuth();
  const location = useLocation();

  if (booting) {
    return (
      <div className="container-fluid d-flex flex-column min-vh-100 p-0">
        <main className="flex-fill d-flex flex-column justify-content-center align-items-center">
          <div className="card border-0" style={{ width: '100%', maxWidth: 520 }}>
            <div className="card-body p-4 text-center text-muted">加载中…</div>
          </div>
        </main>
      </div>
    );
  }

  if (!user) {
    const from = `${location.pathname}${location.search}${location.hash}`;
    return <Navigate to="/login" replace state={{ from }} />;
  }

  if (rootOnly && user.role !== 'root') {
    return <Navigate to="/dashboard" replace />;
  }

  return <>{children}</>;
}
