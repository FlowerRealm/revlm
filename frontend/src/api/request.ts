import type { AxiosRequestConfig } from 'axios';

import { api } from './client';

export async function getData<TResponse>(url: string, config?: AxiosRequestConfig) {
  const res = await api.get<TResponse>(url, config);
  return res.data;
}

export async function postData<TResponse, TRequest = unknown>(
  url: string,
  data?: TRequest,
  config?: AxiosRequestConfig
) {
  const res = await api.post<TResponse>(url, data, config);
  return res.data;
}

export async function putData<TResponse, TRequest = unknown>(
  url: string,
  data?: TRequest,
  config?: AxiosRequestConfig
) {
  const res = await api.put<TResponse>(url, data, config);
  return res.data;
}

export async function deleteData<TResponse>(url: string, config?: AxiosRequestConfig) {
  const res = await api.delete<TResponse>(url, config);
  return res.data;
}
