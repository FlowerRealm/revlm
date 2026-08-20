import type { Meta, StoryObj } from '@storybook/react-vite';

import { ConfigForm } from './ConfigForm';
import { accountEmailTemplate, createAdminUserTemplate } from './configTemplates';

// `ConfigForm` is generic over `TValues`/`TResult`, and the two templates below instantiate it
// differently — `Meta<typeof ConfigForm>` can't express "any instantiation", so this file uses
// the untyped `Meta`/`StoryObj` (each story's `render` is fully typed on its own).
const meta: Meta = {
  title: 'Components/Admin/ConfigForm',
  parameters: { layout: 'centered' },
};

export default meta;

/**
 * `ConfigForm` is driven entirely by a `ConfigTemplate` (see `configTemplates.ts`) — these
 * stories reuse the real templates the app ships (`accountEmailTemplate`, `createAdminUserTemplate`)
 * rather than inventing story-only fixtures. Submitting hits the mocked `/api/*` handlers.
 */
export const SingleField: StoryObj = {
  name: '单字段（邮箱）',
  render: () => (
    <div style={{ width: 420 }}>
      <ConfigForm template={accountEmailTemplate} layout="stack" />
    </div>
  ),
};

export const GridWithChoices: StoryObj = {
  name: '网格布局（含下拉选择）',
  render: () => (
    <div style={{ width: 640 }}>
      <ConfigForm template={createAdminUserTemplate} layout="grid" />
    </div>
  ),
};
