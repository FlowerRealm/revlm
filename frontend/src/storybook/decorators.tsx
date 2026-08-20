import type { ComponentType } from 'react';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import type { InitialEntry } from 'react-router-dom';
import type { Decorator } from '@storybook/react-vite';

import { AuthContext } from '../auth/AuthContext';
import type { AuthState } from '../auth/AuthContext';
import type { User } from '../api/types';

/**
 * Feeds `useAuth()` a fixed value without mounting the real `AuthProvider`
 * (which fires `GET /api/user/self` on mount). Pass `null` for a signed-out view.
 */
export function withAuth(user: User | null): Decorator {
  const value: AuthState = {
    user,
    booting: false,
    loading: false,
    refresh: async () => user,
    login: async () => {},
    register: async () => {},
    logout: async () => {},
  };
  return (Story) => (
    <AuthContext.Provider value={value}>
      <Story />
    </AuthContext.Provider>
  );
}

/** Wraps the story in a `MemoryRouter` for components that call `useLocation`/`Link`/`NavLink`. */
export function withRouter(initialEntries: InitialEntry[] = ['/']): Decorator {
  return (Story) => (
    <MemoryRouter initialEntries={initialEntries}>
      <Story />
    </MemoryRouter>
  );
}

/**
 * Mounts a real layout component (`AppLayout`, `AdminLayout`, `PublicLayout`) as the
 * route parent so the story renders through its actual `<Outlet/>`, matching how it's
 * used in `App.tsx`.
 *
 * For layouts that call `useAuth()`, combine with `withAuth` — and list `withAuth` LAST:
 * Storybook nests decorators with the last one outermost, and the layout this decorator
 * renders needs to be inside `withAuth`'s provider, not just the story content:
 * `decorators: [withLayoutRoute(AppLayout), withAuth(user)]`.
 */
export function withLayoutRoute(Layout: ComponentType, initialEntries: InitialEntry[] = ['/']): Decorator {
  return (Story) => (
    <MemoryRouter initialEntries={initialEntries}>
      <Routes>
        <Route element={<Layout />}>
          <Route path="*" element={<Story />} />
        </Route>
      </Routes>
    </MemoryRouter>
  );
}
