import { useEffect, useMemo, useState } from 'react';
import { Link, NavLink, Outlet } from 'react-router-dom';

import { useAuth } from '../auth/AuthContext';

function userEmail(userEmailValue: string | null | undefined, username: string | null | undefined): string {
  const email = (userEmailValue || '').trim();
  if (email) return email;
  const u = (username || '').trim();
  if (u) return u;
  return '未登录';
}

export function AdminLayout() {
  const { user } = useAuth();
  const [sidebarOpen, setSidebarOpen] = useState(false);

  useEffect(() => {
    document.documentElement.classList.remove('app-html');
    document.body.classList.remove('app-body');
    document.documentElement.classList.add('admin-html');
    document.body.classList.add('admin-body');
    return () => {
      document.documentElement.classList.remove('admin-html');
      document.body.classList.remove('admin-body');
    };
  }, []);

  const closeSidebar = () => setSidebarOpen(false);

  const loginLabel = useMemo(() => userEmail(user?.email, user?.username), [user?.email, user?.username]);

  return (
    <div className="app-shell d-flex flex-grow-1">
      <aside className={`sidebar d-flex flex-column flex-shrink-0 p-3 ${sidebarOpen ? 'show' : ''}`} id="sidebarMenu">
        <Link
          to="/admin/dashboard"
          className="d-flex align-items-center mb-4 mb-md-0 me-md-auto text-decoration-none px-2 mt-2"
          onClick={closeSidebar}
        >
          <div
            className="me-2 d-flex align-items-center justify-content-center flex-shrink-0"
            style={{ width: 36, height: 36 }}
          >
            <img src="/assets/revlm_icon.svg" alt="Revlm" style={{ width: 24, height: 24 }} />
          </div>
          <span className="fs-5 fw-bold text-body tracking-tight">管理后台</span>
        </Link>
        <hr className="text-secondary opacity-50 my-3" />

        <ul className="nav flex-column sidebar-nav mb-0 flex-grow-1 overflow-auto">
          <li>
            <NavLink
              to="/admin/dashboard"
              className={({ isActive }) => `sidebar-link${isActive ? ' active' : ''}`}
              onClick={closeSidebar}
            >
              <i className="ri-dashboard-line"></i> 仪表盘
            </NavLink>
          </li>
          <li>
            <NavLink
              to="/admin/channels"
              className={({ isActive }) => `sidebar-link${isActive ? ' active' : ''}`}
              onClick={closeSidebar}
            >
              <i className="ri-git-merge-line"></i> 上游渠道
            </NavLink>
          </li>
          <li>
            <NavLink
              to="/admin/channel-groups"
              className={({ isActive }) => `sidebar-link${isActive ? ' active' : ''}`}
              onClick={closeSidebar}
            >
              <i className="ri-folder-settings-line"></i> 渠道组
            </NavLink>
          </li>
          <li>
            <NavLink
              to="/admin/users"
              className={({ isActive }) => `sidebar-link${isActive ? ' active' : ''}`}
              onClick={closeSidebar}
            >
              <i className="ri-user-settings-line"></i> 用户管理
            </NavLink>
          </li>
          <li>
            <NavLink
              to="/admin/usage"
              className={({ isActive }) => `sidebar-link${isActive ? ' active' : ''}`}
              onClick={closeSidebar}
            >
              <i className="ri-line-chart-line"></i> 用量统计
            </NavLink>
          </li>
          <li>
            <NavLink
              to="/admin/settings"
              className={({ isActive }) => `sidebar-link${isActive ? ' active' : ''}`}
              onClick={closeSidebar}
            >
              <i className="ri-settings-3-line"></i> 系统设置
            </NavLink>
          </li>

          <li className="mt-4 mb-2 ms-2 text-uppercase text-muted sidebar-section-label">应用</li>
          <li>
            <NavLink
              to="/dashboard"
              className={({ isActive }) => `sidebar-link${isActive ? ' active' : ''}`}
              onClick={closeSidebar}
            >
              <i className="ri-external-link-line"></i> 用户控制台
            </NavLink>
          </li>
        </ul>

        <div className="mt-auto px-2 pb-2">
          <span className="text-muted small opacity-50">Revlm 管理后台</span>
        </div>
      </aside>

      <div className="main-wrapper w-100">
        <header className="top-header">
          <button
            className="btn btn-link text-body d-md-none p-0"
            type="button"
            onClick={() => setSidebarOpen((v) => !v)}
            aria-label="打开侧边栏"
          >
            <i className="ri-menu-line fs-4"></i>
          </button>

          <div className="ms-auto">
            <span className="text-muted small me-2">当前登录</span>
            <span className="fw-bold small text-body">{loginLabel}</span>
          </div>
        </header>

        <main className="content-scrollable">
          <div className="container-fluid" style={{ maxWidth: 1600 }}>
            <Outlet />
          </div>
        </main>
      </div>
    </div>
  );
}
