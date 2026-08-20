import type { Meta, StoryObj } from '@storybook/react-vite';

import { AdminLayout } from './AdminLayout';
import { withAuth, withLayoutRoute } from '../storybook/decorators';
import { rootUser } from '../storybook/fixtures/user';

function PlaceholderPage() {
  return (
    <div className="p-4">
      <h2 className="h4">管理页面内容</h2>
      <p className="text-muted">路由渲染在 &lt;Outlet/&gt; 里，这里只是一个占位页。</p>
    </div>
  );
}

const meta = {
  title: 'Layout/AdminLayout',
  component: AdminLayout,
  parameters: { layout: 'fullscreen' },
} satisfies Meta<typeof AdminLayout>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {
  decorators: [withLayoutRoute(AdminLayout, ['/admin/dashboard']), withAuth(rootUser)],
  render: () => <PlaceholderPage />,
};
