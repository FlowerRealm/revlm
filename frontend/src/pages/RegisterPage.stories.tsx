import type { Meta, StoryObj } from '@storybook/react-vite';

import { RegisterPage } from './RegisterPage';
import { withAuth, withRouter } from '../storybook/decorators';

const meta = {
  title: 'Pages/RegisterPage',
  component: RegisterPage,
  parameters: { layout: 'centered' },
  decorators: [withAuth(null), withRouter(['/register'])],
} satisfies Meta<typeof RegisterPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};

export const WithRoutedError: Story = {
  name: '带路由传入的错误',
  decorators: [withRouter([{ pathname: '/register', state: { error: '账号名已被占用' } }])],
};
