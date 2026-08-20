import type { User } from '../../api/types';

export const regularUser: User = {
  id: 1,
  email: 'alice@example.com',
  username: 'alice',
  role: 'user',
  status: 1,
  groups: ['default'],
};

export const rootUser: User = {
  id: 2,
  email: 'root@example.com',
  username: 'root',
  role: 'root',
  status: 1,
  groups: ['default'],
};
