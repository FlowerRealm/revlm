import type { Meta, StoryObj } from '@storybook/react-vite';

import { PublicLayout } from './PublicLayout';
import { withLayoutRoute } from '../storybook/decorators';

function PlaceholderPage() {
  return (
    <div className="card border-0 p-4">
      <p className="text-muted mb-0">路由渲染在 &lt;Outlet/&gt; 里，这里只是一个占位页。</p>
    </div>
  );
}

const meta = {
  title: 'Layout/PublicLayout',
  component: PublicLayout,
  parameters: { layout: 'fullscreen' },
} satisfies Meta<typeof PublicLayout>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {
  decorators: [withLayoutRoute(PublicLayout, ['/login'])],
  render: () => <PlaceholderPage />,
};
