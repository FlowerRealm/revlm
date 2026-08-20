import type { Meta, StoryObj } from '@storybook/react-vite';

import { SegmentedFrame } from './SegmentedFrame';

const meta = {
  title: 'Components/SegmentedFrame',
  component: SegmentedFrame,
  parameters: { layout: 'centered' },
} satisfies Meta<typeof SegmentedFrame>;

export default meta;
type Story = StoryObj<typeof meta>;

export const TwoSegments: Story = {
  args: {
    children: [
      <div key="a" className="card border-0 p-4" style={{ width: 360 }}>
        第一段
      </div>,
      <div key="b" className="card border-0 p-4" style={{ width: 360 }}>
        第二段
      </div>,
    ],
  },
};

export const SingleSegment: Story = {
  args: {
    children: (
      <div className="card border-0 p-4" style={{ width: 360 }}>
        单段内容
      </div>
    ),
  },
};
