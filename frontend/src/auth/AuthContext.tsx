/* eslint-disable react-refresh/only-export-components */
import { createContext, useCallback, useContext, useEffect, useMemo, useState } from 'react';
import type { ReactNode } from 'react';

import { api } from '../api/client';
import type { APIResponse, User } from '../api/types';

type AuthState = {
  user: User | null;
  booting: boolean;
  loading: boolean;
  refresh: () => Promise<User | null>;
  login: (login: string, password: string) => Promise<void>;
  register: (email: string, username: string, password: string) => Promise<void>;
  logout: () => Promise<void>;
};

const AuthContext = createContext<AuthState | null>(null);

export function AuthProvider({ children }: { children: ReactNode }) {
  const [user, setUser] = useState<User | null>(null);
  const [booting, setBooting] = useState(true);
  const [loading, setLoading] = useState(true);

  const refreshInternal = useCallback(async (markBootDone: boolean) => {
    setLoading(true);
    try {
      const res = await api.get<APIResponse<User>>('/api/user/self');
      if (res.data?.success && res.data.data) {
        const next = res.data.data;
        setUser(next);
        return next;
      }
      setUser(null);
      return null;
    } finally {
      setLoading(false);
      if (markBootDone) setBooting(false);
    }
  }, []);

  const refresh = useCallback(async () => refreshInternal(false), [refreshInternal]);

  const login = useCallback(
    async (login: string, password: string) => {
      setLoading(true);
      try {
        const res = await api.post<APIResponse<User>>('/api/user/login', {
          login,
          password,
        });
        if (!res.data?.success) {
          throw new Error(res.data?.message || '登录失败');
        }
        if (res.data.data) {
          setUser(res.data.data);
        }
        const next = await refresh();
        if (!next) {
          throw new Error('登录失败：会话初始化失败，请重试');
        }
      } finally {
        setLoading(false);
      }
    },
    [refresh]
  );

  const register = useCallback(
    async (email: string, username: string, password: string) => {
      setLoading(true);
      try {
        const res = await api.post<APIResponse<User>>('/api/user/register', {
          email,
          username,
          password,
        });
        if (!res.data?.success) {
          throw new Error(res.data?.message || '注册失败');
        }
        if (res.data.data) {
          setUser(res.data.data);
        }
        const next = await refresh();
        if (!next) {
          throw new Error('注册失败：会话初始化失败，请重试');
        }
      } finally {
        setLoading(false);
      }
    },
    [refresh]
  );

  const logout = useCallback(async () => {
    setLoading(true);
    try {
      await api.get('/api/user/logout');
      setUser(null);
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void refreshInternal(true);
  }, [refreshInternal]);

  const value = useMemo<AuthState>(
    () => ({
      user,
      booting,
      loading,
      refresh,
      login,
      register,
      logout,
    }),
    [booting, loading, login, logout, refresh, register, user]
  );

  return <AuthContext.Provider value={value}>{children}</AuthContext.Provider>;
}

export function useAuth() {
  const ctx = useContext(AuthContext);
  if (!ctx) {
    throw new Error('useAuth must be used within <AuthProvider />');
  }
  return ctx;
}
