import { http, HttpResponse } from 'msw';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { UsageAdminPage } from './UsageAdminPage';
import { withAuth, withRouter } from '../../storybook/decorators';
import { rootUser } from '../../storybook/fixtures/user';
import { ok } from '../../storybook/handlers';

const meta = {
  title: 'Pages/Admin/UsageAdminPage',
  component: UsageAdminPage,
  parameters: { layout: 'padded' },
  decorators: [withAuth(rootUser), withRouter(['/admin/usage'])],
} satisfies Meta<typeof UsageAdminPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};

export const NoEvents: Story = {
  name: '当前范围无事件',
  beforeEach({ msw }) {
    msw.use(
      http.get('/api/admin/request', () =>
        HttpResponse.json(
          ok({
            admin_time_zone: 'Asia/Shanghai',
            now: '2026-08-19 09:12:00',
            start: '2026-08-12',
            end: '2026-08-19',
            limit: 20,
            top_users: [],
            events: [],
            cursor_active: false,
          })
        )
      )
    );
  },
};
