import { http, HttpResponse } from 'msw';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { TokensPage } from './TokensPage';
import { withAuth, withRouter } from '../storybook/decorators';
import { regularUser } from '../storybook/fixtures/user';
import { ok } from '../storybook/handlers';

const meta = {
  title: 'Pages/TokensPage',
  component: TokensPage,
  parameters: { layout: 'padded' },
  decorators: [withAuth(regularUser), withRouter(['/tokens'])],
} satisfies Meta<typeof TokensPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};

export const NoTokens: Story = {
  name: '尚未创建任何 Token',
  beforeEach({ msw }) {
    msw.use(http.get('/api/token', () => HttpResponse.json(ok([]))));
  },
};
