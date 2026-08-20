import { http, HttpResponse } from 'msw';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { AdminDashboardPage } from './AdminDashboardPage';
import { withAuth, withRouter } from '../../storybook/decorators';
import { rootUser } from '../../storybook/fixtures/user';
import { fail } from '../../storybook/handlers';

const meta = {
  title: 'Pages/Admin/AdminDashboardPage',
  component: AdminDashboardPage,
  parameters: { layout: 'padded' },
  decorators: [withAuth(rootUser), withRouter(['/admin/dashboard'])],
} satisfies Meta<typeof AdminDashboardPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};

export const RequestFailed: Story = {
  name: '请求失败',
  beforeEach({ msw }) {
    msw.use(http.get('/api/admin/dashboard', () => HttpResponse.json(fail('加载失败：服务暂不可用'))));
  },
};
