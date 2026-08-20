import type { Meta, StoryObj } from '@storybook/react-vite';

import { UsageSummaryCard } from './UsageSummaryCard';
import { emptyUsageWindow, usageWindow } from '../../storybook/fixtures/usage';

const meta = {
  title: 'Pages/Usage/UsageSummaryCard',
  component: UsageSummaryCard,
  parameters: { layout: 'padded' },
} satisfies Meta<typeof UsageSummaryCard>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {
  args: {
    data: usageWindow,
    rangeSinceText: usageWindow.since,
    rangeUntilText: usageWindow.until,
  },
};

export const Empty: Story = {
  args: {
    data: emptyUsageWindow,
    rangeSinceText: emptyUsageWindow.since,
    rangeUntilText: emptyUsageWindow.until,
  },
};
