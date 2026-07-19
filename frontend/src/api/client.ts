import axios from 'axios';

export const api = axios.create({
  baseURL: import.meta.env.VITE_API_BASE_URL || '',
  withCredentials: true,
  headers: {
    'Cache-Control': 'no-store',
  },
});

api.interceptors.request.use((config) => {
  config.headers = config.headers ?? {};
  const headers = config.headers as Record<string, string>;
  if (!headers['X-Request-Id'] && !headers['x-request-id']) {
    headers['X-Request-Id'] = crypto.randomUUID();
  }
  return config;
});
