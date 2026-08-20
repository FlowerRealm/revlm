import type { Meta, StoryObj } from '@storybook/react-vite';

import { UsageTopUsersCard } from './UsageTopUsersCard';
import { topUsers } from '../../storybook/fixtures/usage';

const meta = {
  title: 'Pages/Usage/UsageTopUsersCard',
  component: UsageTopUsersCard,
  parameters: { layout: 'padded' },
} satisfies Meta<typeof UsageTopUsersCard>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {
  args: { topUsers },
};

export const Empty: Story = {
  args: { topUsers: [] },
};
