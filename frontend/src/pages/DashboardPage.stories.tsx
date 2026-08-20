import { http, HttpResponse } from 'msw';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { DashboardPage } from './DashboardPage';
import { withAuth, withRouter } from '../storybook/decorators';
import { regularUser } from '../storybook/fixtures/user';
import { emptyDashboardData } from '../storybook/fixtures/dashboard';
import { fail, ok } from '../storybook/handlers';

const meta = {
  title: 'Pages/DashboardPage',
  component: DashboardPage,
  parameters: { layout: 'padded' },
  decorators: [withAuth(regularUser), withRouter(['/dashboard'])],
} satisfies Meta<typeof DashboardPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};

export const NoUsageYet: Story = {
  name: '今日尚无用量',
  beforeEach({ msw }) {
    msw.use(http.get('/api/dashboard', () => HttpResponse.json(ok(emptyDashboardData))));
  },
};

export const RequestFailed: Story = {
  name: '请求失败',
  beforeEach({ msw }) {
    msw.use(http.get('/api/dashboard', () => HttpResponse.json(fail('加载失败：服务暂不可用'))));
  },
};
