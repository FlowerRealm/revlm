import type { Meta, StoryObj } from '@storybook/react-vite';

import { UsageAdminTopUsersCard } from './UsageAdminTopUsersCard';
import { adminTopUsers } from '../../../storybook/fixtures/adminUsage';

const meta = {
  title: 'Pages/Admin/Usage/UsageAdminTopUsersCard',
  component: UsageAdminTopUsersCard,
  parameters: { layout: 'padded' },
} satisfies Meta<typeof UsageAdminTopUsersCard>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {
  args: { topUsers: adminTopUsers },
};

export const Empty: Story = {
  args: { topUsers: [] },
};
