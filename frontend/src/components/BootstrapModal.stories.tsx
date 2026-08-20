import { useEffect } from 'react';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { BootstrapModal } from './BootstrapModal';
import { showModalById } from './modal';

/**
 * `BootstrapModal` renders Bootstrap's `.modal` markup, which is `display: none` until
 * Bootstrap's own JS toggles it. This wrapper calls `showModalById` (the same helper the app
 * uses) once the vendored `bootstrap.bundle.min.js` script (loaded via `.storybook/preview-head.html`)
 * has attached `window.bootstrap`.
 */
function AutoOpen({ id, children }: { id: string; children: React.ReactNode }) {
  useEffect(() => {
    const timer = window.setTimeout(() => showModalById(id), 50);
    return () => window.clearTimeout(timer);
  }, [id]);
  return <>{children}</>;
}

const meta = {
  title: 'Components/BootstrapModal',
  component: BootstrapModal,
  parameters: { layout: 'fullscreen' },
} satisfies Meta<typeof BootstrapModal>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Basic: Story = {
  args: {
    id: 'storybook-basic-modal',
    title: '示例弹窗',
    children: <p className="mb-0">这是弹窗正文内容。</p>,
  },
  render: (args) => (
    <AutoOpen id={args.id}>
      <BootstrapModal {...args} />
    </AutoOpen>
  ),
};

export const WithFooter: Story = {
  args: {
    id: 'storybook-footer-modal',
    title: '带底部操作的弹窗',
    children: <p className="mb-0">确认要执行此操作吗？</p>,
    footer: (
      <>
        <button type="button" className="btn btn-secondary" data-bs-dismiss="modal">
          取消
        </button>
        <button type="button" className="btn btn-primary">
          确认
        </button>
      </>
    ),
  },
  render: (args) => (
    <AutoOpen id={args.id}>
      <BootstrapModal {...args} />
    </AutoOpen>
  ),
};
