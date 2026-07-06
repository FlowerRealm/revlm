export type APIResponse<T> = {
  success: boolean;
  message?: string;
  data?: T;
};

export type User = {
  id: number;
  email?: string;
  username?: string;
  role?: string;
  status?: number;
  groups?: string[];

  mode?: 'business';
};
