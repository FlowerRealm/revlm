import { http, HttpResponse } from 'msw';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { PluginsPage } from './PluginsPage';
import { withAuth, withRouter } from '../../storybook/decorators';
import { rootUser } from '../../storybook/fixtures/user';
import { ok } from '../../storybook/handlers';

const meta = {
  title: 'Pages/Admin/PluginsPage',
  component: PluginsPage,
  parameters: { layout: 'padded' },
  decorators: [withAuth(rootUser), withRouter(['/admin/plugins'])],
} satisfies Meta<typeof PluginsPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};

export const NoPlugins: Story = {
  name: '尚未安装任何插件',
  beforeEach({ msw }) {
    msw.use(http.get('/api/admin/plugins', () => HttpResponse.json(ok([]))));
  },
};
