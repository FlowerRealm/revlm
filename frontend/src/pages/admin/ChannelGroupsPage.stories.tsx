import { http, HttpResponse } from 'msw';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { ChannelGroupsPage } from './ChannelGroupsPage';
import { withAuth, withRouter } from '../../storybook/decorators';
import { rootUser } from '../../storybook/fixtures/user';
import { ok } from '../../storybook/handlers';

const meta = {
  title: 'Pages/Admin/ChannelGroupsPage',
  component: ChannelGroupsPage,
  parameters: { layout: 'padded' },
  decorators: [withAuth(rootUser), withRouter(['/admin/channel-groups'])],
} satisfies Meta<typeof ChannelGroupsPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};

export const NoGroups: Story = {
  name: '尚无渠道组',
  beforeEach({ msw }) {
    msw.use(http.get('/api/admin/channel-groups', () => HttpResponse.json(ok([]))));
  },
};
