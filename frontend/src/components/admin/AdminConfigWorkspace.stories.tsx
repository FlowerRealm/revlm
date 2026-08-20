import { useState } from 'react';
import type { Meta, StoryObj } from '@storybook/react-vite';

import {
  AdminConfigDetail,
  AdminConfigList,
  AdminConfigListItem,
  AdminConfigSection,
  AdminConfigSectionNav,
  AdminConfigSidebar,
  AdminConfigWorkspace,
} from './AdminConfigWorkspace';

const meta = {
  title: 'Components/Admin/AdminConfigWorkspace',
  component: AdminConfigWorkspace,
  parameters: { layout: 'fullscreen' },
} satisfies Meta<typeof AdminConfigWorkspace>;

export default meta;
type Story = StoryObj<typeof meta>;

/** The full sidebar + detail composition, as used by `AdminPage`'s config sections. */
function FullWorkspaceDemo() {
  const [activeID, setActiveID] = useState('sms');
  const items = [
    { id: 'sms', title: '短信通道', meta: '3 个供应商', hint: '优先级路由' },
    { id: 'email', title: '邮件通道', meta: '1 个供应商' },
    { id: 'webhook', title: 'Webhook', meta: '未配置', badge: <span className="badge bg-secondary">禁用</span> },
  ];
  return (
    <AdminConfigWorkspace
      sidebar={
        <AdminConfigSidebar title="通道列表" description="选择一个通道查看详情">
          <AdminConfigList>
            {items.map((item) => (
              <AdminConfigListItem
                key={item.id}
                active={item.id === activeID}
                title={item.title}
                meta={item.meta}
                hint={item.hint}
                badge={item.badge}
                onClick={() => setActiveID(item.id)}
              />
            ))}
          </AdminConfigList>
        </AdminConfigSidebar>
      }
      detail={
        <AdminConfigDetail
          header={<h5 className="mb-0 fw-semibold">{items.find((i) => i.id === activeID)?.title}</h5>}
          sectionNav={
            <AdminConfigSectionNav
              items={[
                { id: 'basic', label: '基本信息' },
                { id: 'advanced', label: '高级设置', hint: '可选' },
              ]}
            />
          }
        >
          <AdminConfigSection id="basic" title="基本信息" description="通道的基础配置">
            <div className="text-muted small">（表单内容占位）</div>
          </AdminConfigSection>
          <AdminConfigSection id="advanced" title="高级设置">
            <div className="text-muted small">（表单内容占位）</div>
          </AdminConfigSection>
        </AdminConfigDetail>
      }
    />
  );
}

export const FullWorkspace: Story = {
  args: { sidebar: null, detail: null },
  render: () => <FullWorkspaceDemo />,
};
