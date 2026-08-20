import { useState } from 'react';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { DateRangePicker, SelectPicker, type DateRange } from './DateRangePicker';

function Demo({ loading }: { loading?: boolean }) {
  const [range, setRange] = useState<DateRange>({ start: '2026-08-12', end: '2026-08-19' });
  return (
    <div>
      <DateRangePicker start={range.start} end={range.end} loading={loading} onChange={setRange} />
      <div className="text-muted small mt-2">
        当前值：{range.start || '（空）'} ~ {range.end || '（空）'}
      </div>
    </div>
  );
}

const meta = {
  title: 'Components/DateRangePicker',
  component: DateRangePicker,
  parameters: { layout: 'centered' },
} satisfies Meta<typeof DateRangePicker>;

export default meta;
type Story = StoryObj<typeof meta>;

// `render` owns the actual interactive state below; `args` only seeds the initial values TS/the
// Controls panel expect from `meta.component`'s required props.
const baseArgs = { start: '2026-08-12', end: '2026-08-19', onChange: () => {} };

export const Default: Story = {
  args: baseArgs,
  render: () => <Demo />,
};

export const Loading: Story = {
  args: { ...baseArgs, loading: true },
  render: () => <Demo loading />,
};

function SelectPickerDemo() {
  const [value, setValue] = useState<'hour' | 'day'>('hour');
  return (
    <SelectPicker<'hour' | 'day'>
      value={value}
      onChange={setValue}
      label="粒度"
      options={[
        { value: 'hour', label: '按小时' },
        { value: 'day', label: '按天' },
      ]}
    />
  );
}

export const SelectPickerStory: StoryObj = {
  name: 'SelectPicker（同文件导出）',
  render: () => <SelectPickerDemo />,
};
