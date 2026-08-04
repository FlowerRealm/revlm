import { Suspense, lazy } from 'react';
import { Navigate, Route, Routes } from 'react-router-dom';

import { useAuth } from '../auth/AuthContext';
import { SegmentedFrame } from '../components/SegmentedFrame';

const AdminDashboardPage = lazy(() =>
  import('./admin/AdminDashboardPage').then((m) => ({ default: m.AdminDashboardPage }))
);
const ChannelsPage = lazy(() => import('./admin/ChannelsPage').then((m) => ({ default: m.ChannelsPage })));
const ChannelGroupsPage = lazy(() =>
  import('./admin/ChannelGroupsPage').then((m) => ({ default: m.ChannelGroupsPage }))
);
const UsageAdminPage = lazy(() => import('./admin/UsageAdminPage').then((m) => ({ default: m.UsageAdminPage })));
const UsersPage = lazy(() => import('./admin/UsersPage').then((m) => ({ default: m.UsersPage })));

function AdminRouteFallback() {
  return <div className="text-muted">加载中…</div>;
}

export function AdminPage() {
  const { user } = useAuth();

  if (user?.role !== 'root') {
    return (
      <div className="fade-in-up">
        <SegmentedFrame>
          <div className="alert alert-danger mb-0" role="alert">
            <span className="me-2 material-symbols-rounded">report</span> 权限不足（需要 root）。
          </div>
        </SegmentedFrame>
      </div>
    );
  }

  return (
    <Suspense fallback={<AdminRouteFallback />}>
      <Routes>
        <Route index element={<Navigate to="/admin/dashboard" replace />} />
        <Route path="dashboard" element={<AdminDashboardPage />} />
        <Route path="channels" element={<ChannelsPage />} />
        <Route path="channel-groups" element={<ChannelGroupsPage />} />
        <Route path="users" element={<UsersPage />} />
        <Route path="usage" element={<UsageAdminPage />} />
        <Route path="*" element={<Navigate to="/admin/dashboard" replace />} />
      </Routes>
    </Suspense>
  );
}
