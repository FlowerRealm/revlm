import { http, HttpResponse } from 'msw';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { UsersPage } from './UsersPage';
import { withAuth, withRouter } from '../../storybook/decorators';
import { rootUser } from '../../storybook/fixtures/user';
import { ok } from '../../storybook/handlers';

const meta = {
  title: 'Pages/Admin/UsersPage',
  component: UsersPage,
  parameters: { layout: 'padded' },
  decorators: [withAuth(rootUser), withRouter(['/admin/users'])],
} satisfies Meta<typeof UsersPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};

export const NoUsers: Story = {
  name: '尚无用户',
  beforeEach({ msw }) {
    msw.use(http.get('/api/admin/users', () => HttpResponse.json(ok([]))));
  },
};
