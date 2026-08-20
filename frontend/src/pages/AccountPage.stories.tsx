import type { Meta, StoryObj } from '@storybook/react-vite';

import { AccountPage } from './AccountPage';
import { withAuth, withRouter } from '../storybook/decorators';
import { regularUser } from '../storybook/fixtures/user';

const meta = {
  title: 'Pages/AccountPage',
  component: AccountPage,
  parameters: { layout: 'padded' },
  decorators: [withAuth(regularUser), withRouter(['/account'])],
} satisfies Meta<typeof AccountPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};
