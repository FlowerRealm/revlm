import { useEffect } from 'react';
import { Link, NavLink, Outlet } from 'react-router-dom';

export function PublicLayout() {
  useEffect(() => {
    document.documentElement.classList.remove('admin-html');
    document.body.classList.remove('admin-body');
    document.documentElement.classList.remove('app-html');
    document.body.classList.remove('app-body');
  }, []);

  return (
    <div className="container-fluid d-flex flex-column min-vh-100 p-0">
      <header className="simple-header d-flex flex-wrap justify-content-center py-3 mb-4">
        <Link to="/" className="d-flex align-items-center mb-3 mb-md-0 me-md-auto text-body text-decoration-none ms-4">
          <div
            className="me-2 d-flex align-items-center justify-content-center flex-shrink-0"
            style={{ width: 32, height: 32 }}
          >
            <img src="/assets/revlm_icon.svg" alt="Revlm" style={{ width: 22, height: 22 }} />
          </div>
          <span className="fs-4 fw-bold tracking-tight">Revlm</span>
        </Link>

        <ul className="nav nav-pills me-4">
          <li className="nav-item">
            <NavLink
              to="/login"
              className={({ isActive }) => `nav-link${isActive ? ' active rounded-pill px-4' : ' text-secondary'}`}
            >
              登录
            </NavLink>
          </li>
          <li className="nav-item">
            <NavLink
              to="/register"
              className={({ isActive }) => `nav-link${isActive ? ' active rounded-pill px-4' : ' text-secondary'}`}
            >
              注册
            </NavLink>
          </li>
        </ul>
      </header>

      <main className="flex-fill d-flex flex-column justify-content-center align-items-center">
        <div className="w-100" style={{ maxWidth: 520 }}>
          <Outlet />
        </div>
      </main>
    </div>
  );
}
