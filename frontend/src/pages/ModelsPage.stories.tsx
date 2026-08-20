import { http, HttpResponse } from 'msw';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { ModelsPage } from './ModelsPage';
import { withAuth, withRouter } from '../storybook/decorators';
import { regularUser } from '../storybook/fixtures/user';
import { fail, ok } from '../storybook/handlers';

const meta = {
  title: 'Pages/ModelsPage',
  component: ModelsPage,
  parameters: { layout: 'padded' },
  decorators: [withAuth(regularUser), withRouter(['/models'])],
} satisfies Meta<typeof ModelsPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};

export const NoModels: Story = {
  name: '未加载任何插件模型',
  beforeEach({ msw }) {
    msw.use(http.get('/api/user/models/detail', () => HttpResponse.json(ok([]))));
  },
};

export const RequestFailed: Story = {
  name: '请求失败',
  beforeEach({ msw }) {
    msw.use(http.get('/api/user/models/detail', () => HttpResponse.json(fail('加载失败：服务暂不可用'))));
  },
};
