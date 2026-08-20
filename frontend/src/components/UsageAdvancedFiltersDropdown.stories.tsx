import { useState } from 'react';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { UsageAdvancedFiltersDropdown } from './UsageAdvancedFiltersDropdown';

/** Click "高级筛选" to open the popover — closed by default, like on the usage pages. */
function Demo({ disabled }: { disabled?: boolean }) {
  const [user, setUser] = useState('');
  const [channel, setChannel] = useState('');
  const [model, setModel] = useState('');
  return (
    <UsageAdvancedFiltersDropdown
      disabled={disabled}
      toggleTestId="storybook-advanced-filters"
      fields={[
        { inputId: 'f-user', label: '用户', placeholder: '邮箱或 ID', value: user, onChange: setUser },
        { inputId: 'f-channel', label: '渠道', placeholder: '渠道名或 ID', value: channel, onChange: setChannel },
        { inputId: 'f-model', label: '模型', placeholder: '模型 ID', value: model, onChange: setModel },
      ]}
    />
  );
}

const meta = {
  title: 'Components/UsageAdvancedFiltersDropdown',
  component: UsageAdvancedFiltersDropdown,
  parameters: { layout: 'centered' },
} satisfies Meta<typeof UsageAdvancedFiltersDropdown>;

export default meta;
type Story = StoryObj<typeof meta>;

const baseArgs = { toggleTestId: 'storybook-advanced-filters', fields: [] };

export const ClosedByDefault: Story = {
  args: baseArgs,
  render: () => <Demo />,
};

export const Disabled: Story = {
  args: { ...baseArgs, disabled: true },
  render: () => <Demo disabled />,
};
