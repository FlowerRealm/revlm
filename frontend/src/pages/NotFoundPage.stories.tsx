import type { Meta, StoryObj } from '@storybook/react-vite';

import { NotFoundPage } from './NotFoundPage';
import { withRouter } from '../storybook/decorators';

const meta = {
  title: 'Pages/NotFoundPage',
  component: NotFoundPage,
  parameters: { layout: 'fullscreen' },
  decorators: [withRouter(['/some/unknown/path'])],
} satisfies Meta<typeof NotFoundPage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};
