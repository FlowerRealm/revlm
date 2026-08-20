import { http, HttpResponse } from 'msw';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { ChannelsPage } from './ChannelsPage';
import { withAuth, withRouter } from '../../storybook/decorators';
import { rootUser } from '../../storybook/fixtures/user';
import { ok } from '../../storybook/handlers';

const meta = {
  title: 'Pages/Admin/ChannelsPage',
  component: ChannelsPage,
  parameters: { layout: 'padded' },
  decorators: [withAuth(rootUser), withRouter(['/admin/channels'])],
} satisfies Meta<typeof ChannelsPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};

export const NoChannels: Story = {
  name: '尚未接入任何渠道',
  beforeEach({ msw }) {
    msw.use(
      http.get('/api/channel/page', () =>
        HttpResponse.json(
          ok({
            admin_time_zone: 'Asia/Shanghai',
            start: '2026-08-12',
            end: '2026-08-19',
            overview: { requests: 0, usd: '0', avg_first_token_latency: '0' },
            channels: [],
          })
        )
      )
    );
  },
};
