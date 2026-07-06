import { Suspense, lazy } from 'react';
import { Navigate, Route, Routes } from 'react-router-dom';

import { RequireAuth } from './auth/RequireAuth';
import { useAuth } from './auth/AuthContext';
import { AdminLayout } from './layout/AdminLayout';
import { AppLayout } from './layout/AppLayout';
import { PublicLayout } from './layout/PublicLayout';

const AdminPage = lazy(() => import('./pages/AdminPage').then((m) => ({ default: m.AdminPage })));
const AccountPage = lazy(() => import('./pages/AccountPage').then((m) => ({ default: m.AccountPage })));
const DashboardPage = lazy(() => import('./pages/DashboardPage').then((m) => ({ default: m.DashboardPage })));
const LoginPage = lazy(() => import('./pages/LoginPage').then((m) => ({ default: m.LoginPage })));
const ModelsPage = lazy(() => import('./pages/ModelsPage').then((m) => ({ default: m.ModelsPage })));
const NotFoundPage = lazy(() => import('./pages/NotFoundPage').then((m) => ({ default: m.NotFoundPage })));
const RegisterPage = lazy(() => import('./pages/RegisterPage').then((m) => ({ default: m.RegisterPage })));
const TokenCreatedPage = lazy(() => import('./pages/TokenCreatedPage').then((m) => ({ default: m.TokenCreatedPage })));
const TokensPage = lazy(() => import('./pages/TokensPage').then((m) => ({ default: m.TokensPage })));
const TopupPage = lazy(() => import('./pages/TopupPage').then((m) => ({ default: m.TopupPage })));
const UsagePage = lazy(() => import('./pages/UsagePage').then((m) => ({ default: m.UsagePage })));

function RouteFallback() {
  return <div className="container py-5 text-muted">加载中…</div>;
}

function HomeRedirect() {
  const { user, booting } = useAuth();
  if (booting) return null;
  if (user) return <Navigate to="/dashboard" replace />;
  return <Navigate to="/login" replace state={{ from: '/dashboard' }} />;
}

export function App() {
  const { booting } = useAuth();
  if (booting) return null;

  return (
    <Suspense fallback={<RouteFallback />}>
      <Routes>
        <Route path="/" element={<HomeRedirect />} />
        <Route element={<PublicLayout />}>
          <Route path="/login" element={<LoginPage />} />
          <Route path="/register" element={<RegisterPage />} />
        </Route>

        <Route
          element={
            <RequireAuth>
              <AppLayout />
            </RequireAuth>
          }
        >
          <Route path="/dashboard" element={<DashboardPage />} />
          <Route path="/tokens" element={<TokensPage />} />
          <Route path="/tokens/created" element={<TokenCreatedPage />} />
          <Route path="/models" element={<ModelsPage />} />
          <Route path="/usage" element={<UsagePage />} />
          <Route path="/account" element={<AccountPage />} />
          <Route path="/topup" element={<TopupPage />} />
        </Route>

        <Route
          element={
            <RequireAuth rootOnly>
              <AdminLayout />
            </RequireAuth>
          }
        >
          <Route path="/admin/*" element={<AdminPage />} />
        </Route>

        <Route path="*" element={<NotFoundPage />} />
      </Routes>
    </Suspense>
  );
}
