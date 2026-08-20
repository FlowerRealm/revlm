import type { Meta, StoryObj } from '@storybook/react-vite';

import { AdminPage } from './AdminPage';
import { withAuth, withRouter } from '../storybook/decorators';
import { regularUser, rootUser } from '../storybook/fixtures/user';

/**
 * `AdminPage` is otherwise just a `<Routes>` switch onto the pages under `pages/admin/` — each
 * of those has its own story. The one state worth showing here is the permission-denied banner,
 * which only this component renders.
 */
const meta = {
  title: 'Pages/AdminPage',
  component: AdminPage,
  parameters: { layout: 'padded' },
} satisfies Meta<typeof AdminPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const NonRootUserSeesPermissionDenied: Story = {
  name: '非 root 用户 · 权限不足',
  decorators: [withAuth(regularUser), withRouter(['/admin/dashboard'])],
};

export const RootUserRedirectsToDashboard: Story = {
  name: 'root 用户 · 跳转到仪表盘',
  decorators: [withAuth(rootUser), withRouter(['/admin'])],
};
