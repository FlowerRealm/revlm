import type { Meta, StoryObj } from '@storybook/react-vite';

import { AppLayout } from './AppLayout';
import { withAuth, withLayoutRoute } from '../storybook/decorators';
import { regularUser, rootUser } from '../storybook/fixtures/user';

function PlaceholderPage() {
  return (
    <div className="p-4">
      <h2 className="h4">页面内容</h2>
      <p className="text-muted">路由渲染在 &lt;Outlet/&gt; 里，这里只是一个占位页。</p>
    </div>
  );
}

const meta = {
  title: 'Layout/AppLayout',
  component: AppLayout,
  parameters: { layout: 'fullscreen' },
} satisfies Meta<typeof AppLayout>;

export default meta;
type Story = StoryObj<typeof meta>;

export const RegularUser: Story = {
  decorators: [withLayoutRoute(AppLayout, ['/dashboard']), withAuth(regularUser)],
  render: () => <PlaceholderPage />,
};

export const RootUser: Story = {
  decorators: [withLayoutRoute(AppLayout, ['/dashboard']), withAuth(rootUser)],
  render: () => <PlaceholderPage />,
};
