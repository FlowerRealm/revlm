import type { Meta, StoryObj } from '@storybook/react-vite';

import { LoginPage } from './LoginPage';
import { withAuth, withRouter } from '../storybook/decorators';

const meta = {
  title: 'Pages/LoginPage',
  component: LoginPage,
  parameters: { layout: 'centered' },
  decorators: [withAuth(null)],
} satisfies Meta<typeof LoginPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {
  decorators: [withRouter(['/login'])],
};

export const WithNotice: Story = {
  name: '带提示（如刚注册成功）',
  decorators: [withRouter([{ pathname: '/login', state: { notice: '注册成功，请登录' } }])],
};

export const WithRoutedError: Story = {
  name: '带路由传入的错误（如会话过期）',
  decorators: [withRouter([{ pathname: '/login', state: { error: 'session_expired' } }])],
};
