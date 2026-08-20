import type { Meta, StoryObj } from '@storybook/react-vite';

import { UsageAdminSummaryCard } from './UsageAdminSummaryCard';
import { adminUsageWindow } from '../../../storybook/fixtures/adminUsage';

const meta = {
  title: 'Pages/Admin/Usage/UsageAdminSummaryCard',
  component: UsageAdminSummaryCard,
  parameters: { layout: 'padded' },
} satisfies Meta<typeof UsageAdminSummaryCard>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {
  args: { windowStats: adminUsageWindow },
};
