import type { Meta, StoryObj } from '@storybook/react-vite';

import { DividedStack } from './DividedStack';

const meta = {
  title: 'Components/DividedStack',
  component: DividedStack,
  parameters: { layout: 'centered' },
} satisfies Meta<typeof DividedStack>;

export default meta;
type Story = StoryObj<typeof meta>;

export const ThreeItems: Story = {
  args: {
    children: [<div key="a">第一项</div>, <div key="b">第二项</div>, <div key="c">第三项</div>],
  },
};

export const SkipsEmptyChildren: Story = {
  name: '跳过空/假值子节点',
  args: {
    children: [<div key="a">仅这一项渲染</div>, null, '', undefined],
  },
};
