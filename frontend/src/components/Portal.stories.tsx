import type { Meta, StoryObj } from '@storybook/react-vite';

import { Portal } from './Portal';

const meta = {
  title: 'Components/Portal',
  component: Portal,
  parameters: { layout: 'centered' },
} satisfies Meta<typeof Portal>;

export default meta;
type Story = StoryObj<typeof meta>;

/**
 * `Portal` renders its children into `document.body` rather than in place. The box below stays
 * where it's declared in the tree; open devtools to see it's actually a sibling of `#root`, not
 * a descendant of this story's wrapper.
 */
export const RendersIntoDocumentBody: Story = {
  args: {
    children: (
      <div
        className="card border-0 shadow p-3"
        style={{ position: 'fixed', top: 24, right: 24, width: 240, zIndex: 2000 }}
      >
        我是通过 Portal 挂到 document.body 的内容
      </div>
    ),
  },
};
