import { http, HttpResponse } from 'msw';

import type { APIResponse } from '../api/types';
import { regularUser } from './fixtures/user';
import { dashboardData } from './fixtures/dashboard';
import { billingBalance } from './fixtures/billing';
import { usageEventDetail, usageEvents, usageTimeSeries, usageWindow } from './fixtures/usage';
import { userTokenChannel, userTokens } from './fixtures/tokens';
import { pluginModels } from './fixtures/models';
import { adminChannelGroupDetail, adminChannelGroups, adminDashboard, adminUsers } from './fixtures/admin';
import {
  adminUsageEventDetail,
  adminUsageEvents,
  adminUsageTimeSeries,
  adminUsageWindow,
  adminTopUsers,
} from './fixtures/adminUsage';
import { channelItems, channelTimeSeries } from './fixtures/channels';
import { pluginInstallations } from './fixtures/plugins';

/** Wraps a payload in the `{success, data}` envelope every endpoint in `src/api/` returns. */
export function ok<T>(data: T): APIResponse<T> {
  return { success: true, data };
}

/** Mirrors the `{success: false, message}` shape the frontend treats as a request failure. */
export function fail(message: string): APIResponse<never> {
  return { success: false, message };
}

/**
 * Baseline "happy path" handlers for every endpoint the console calls. Installed once in
 * `.storybook/preview.ts` so any story that doesn't need a special network scenario just
 * renders against sane defaults; stories that do (empty/error/loading) override one handler
 * with `beforeEach({ msw }) { msw.use(...) }`.
 */
export const handlers = [
  http.get('/api/user/self', () => HttpResponse.json(ok(regularUser))),
  http.get('/api/dashboard', () => HttpResponse.json(ok(dashboardData))),
  http.get('/api/billing/balance', () => HttpResponse.json(ok(billingBalance))),

  http.get('/api/request/windows', () => HttpResponse.json(ok({ now: '2026-08-19 09:12:00', windows: [usageWindow] }))),
  http.get('/api/request/events', () => HttpResponse.json(ok({ events: usageEvents, next_before_id: null }))),
  http.get('/api/request/events/:id/detail', () => HttpResponse.json(ok(usageEventDetail))),
  http.get('/api/request/timeseries', () =>
    HttpResponse.json(
      ok({ start: '2026-08-12', end: '2026-08-19', granularity: 'hour' as const, points: usageTimeSeries })
    )
  ),

  http.get('/api/token', () => HttpResponse.json(ok(userTokens))),
  http.post('/api/token', () => HttpResponse.json(ok({ token_id: 4, token: 'rlm-mock-token' }))),
  http.post('/api/token/:id/rotate', () => HttpResponse.json(ok({ token_id: 1, token: 'rlm-mock-token' }))),
  http.get('/api/token/:id/reveal', () => HttpResponse.json(ok({ token_id: 1, token: 'rlm-mock-token' }))),
  http.post('/api/token/:id/revoke', () => HttpResponse.json(ok(undefined))),
  http.delete('/api/token/:id', () => HttpResponse.json(ok(undefined))),
  http.get('/api/token/:id/channel', () => HttpResponse.json(ok(userTokenChannel))),
  http.put('/api/token/:id/channel', () => HttpResponse.json(ok(undefined))),

  http.get('/api/user/models/detail', () => HttpResponse.json(ok(pluginModels))),

  http.post('/api/account/email', () => HttpResponse.json(ok({ force_logout: false }))),
  http.post('/api/account/password', () => HttpResponse.json(ok({ force_logout: true }))),

  http.get('/api/channel/page', () =>
    HttpResponse.json(
      ok({
        admin_time_zone: 'Asia/Shanghai',
        start: '2026-08-12',
        end: '2026-08-19',
        overview: { requests: 12480, usd: '842.10', avg_first_token_latency: '0.71' },
        channels: channelItems,
      })
    )
  ),
  http.get('/api/channel/:id/timeseries', () =>
    HttpResponse.json(
      ok({
        admin_time_zone: 'Asia/Shanghai',
        channel_id: 3,
        start: '2026-08-12',
        end: '2026-08-19',
        granularity: 'hour' as const,
        points: channelTimeSeries,
      })
    )
  ),
  http.post('/api/channel', () => HttpResponse.json(ok({ id: 5 }))),
  http.put('/api/channel', () => HttpResponse.json(ok(undefined))),
  http.delete('/api/channel/:id', () => HttpResponse.json(ok(undefined))),

  http.get('/api/admin/dashboard', () => HttpResponse.json(ok(adminDashboard))),

  http.get('/api/admin/plugins', () => HttpResponse.json(ok(pluginInstallations))),
  http.post('/api/admin/plugins/upload', () => HttpResponse.json(ok(undefined))),
  http.post('/api/admin/plugins/:id/enable', () => HttpResponse.json(ok(undefined))),
  http.post('/api/admin/plugins/:id/disable', () => HttpResponse.json(ok(undefined))),
  http.delete('/api/admin/plugins/:id', () => HttpResponse.json(ok(undefined))),

  http.get('/api/admin/users', () => HttpResponse.json(ok(adminUsers))),
  http.post('/api/admin/users', () => HttpResponse.json(ok({ id: 4 }))),
  http.put('/api/admin/users/:id', () => HttpResponse.json(ok(undefined))),
  http.post('/api/admin/users/:id/password', () => HttpResponse.json(ok(undefined))),
  http.post('/api/admin/users/:id/balance', () => HttpResponse.json(ok({ balance_usd: 12.4 }))),
  http.delete('/api/admin/users/:id', () => HttpResponse.json(ok(undefined))),

  http.get('/api/admin/channel-groups', () => HttpResponse.json(ok(adminChannelGroups))),
  http.post('/api/admin/channel-groups', () => HttpResponse.json(ok({ id: 3 }))),
  http.get('/api/admin/channel-groups/:id/detail', () => HttpResponse.json(ok(adminChannelGroupDetail))),
  http.get('/api/admin/channel-groups/:id/pointer', () =>
    HttpResponse.json(ok({ group_id: 1, channel_id: 3, channel_name: 'anthropic-primary', pinned: false }))
  ),
  http.put('/api/admin/channel-groups/:id/pointer', () => HttpResponse.json(ok(undefined))),
  http.put('/api/admin/channel-groups/:id', () => HttpResponse.json(ok(undefined))),
  http.delete('/api/admin/channel-groups/:id', () => HttpResponse.json(ok(undefined))),
  http.put('/api/admin/channel-groups/:id/default', () => HttpResponse.json(ok(undefined))),
  http.post('/api/admin/channel-groups/:id/children/channels', () => HttpResponse.json(ok(undefined))),
  http.delete('/api/admin/channel-groups/:id/children/channels/:channelId', () => HttpResponse.json(ok(undefined))),
  http.post('/api/admin/channel-groups/:id/children/reorder', () => HttpResponse.json(ok(undefined))),

  http.get('/api/admin/request', () =>
    HttpResponse.json(
      ok({
        admin_time_zone: 'Asia/Shanghai',
        now: '2026-08-19 09:12:00',
        start: '2026-08-12',
        end: '2026-08-19',
        limit: 20,
        window: adminUsageWindow,
        top_users: adminTopUsers,
        events: adminUsageEvents,
        cursor_active: false,
      })
    )
  ),
  http.get('/api/admin/request/events/:id/detail', () => HttpResponse.json(ok(adminUsageEventDetail))),
  http.get('/api/admin/request/timeseries', () =>
    HttpResponse.json(
      ok({
        admin_time_zone: 'Asia/Shanghai',
        start: '2026-08-12',
        end: '2026-08-19',
        granularity: 'hour' as const,
        points: adminUsageTimeSeries,
      })
    )
  ),
];

/** A handler for `path` that fails the way the frontend's `APIResponse.success === false` path expects. */
export function failureHandler(method: 'get' | 'post' | 'put' | 'delete', path: string, message: string) {
  return http[method](path, () => HttpResponse.json(fail(message)));
}

/** A handler for `path` that simulates a transport-level error (network down, 5xx with no body). */
export function networkErrorHandler(method: 'get' | 'post' | 'put' | 'delete', path: string) {
  return http[method](path, () => HttpResponse.error());
}
