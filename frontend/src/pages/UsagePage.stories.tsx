import { http, HttpResponse } from 'msw';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { UsagePage } from './UsagePage';
import { withAuth, withRouter } from '../storybook/decorators';
import { regularUser } from '../storybook/fixtures/user';
import { fail, ok } from '../storybook/handlers';

const meta = {
  title: 'Pages/UsagePage',
  component: UsagePage,
  parameters: { layout: 'padded' },
  decorators: [withAuth(regularUser), withRouter(['/usage'])],
} satisfies Meta<typeof UsagePage>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};

export const NoEvents: Story = {
  name: '当前范围无事件',
  beforeEach({ msw }) {
    msw.use(
      http.get('/api/request/windows', () =>
        HttpResponse.json(
          ok({
            now: '2026-08-19 09:12:00',
            windows: [
              {
                window: '',
                since: '2026-08-19 00:00:00',
                until: '2026-08-19 09:12:00',
                requests: 0,
                rpm: 0,
                first_token_samples: 0,
                avg_first_token_latency: 0,
                usd: '0',
              },
            ],
          })
        )
      ),
      http.get('/api/request/events', () => HttpResponse.json(ok({ events: [], next_before_id: null })))
    );
  },
};

export const RequestFailed: Story = {
  name: '请求失败',
  beforeEach({ msw }) {
    msw.use(http.get('/api/request/windows', () => HttpResponse.json(fail('加载失败：服务暂不可用'))));
  },
};
