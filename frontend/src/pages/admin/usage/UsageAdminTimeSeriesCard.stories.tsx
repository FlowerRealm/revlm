import { useState } from 'react';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { UsageAdminTimeSeriesCard } from './UsageAdminTimeSeriesCard';
import { usageAdminFieldOptions, usageAdminGranularityOptions } from './usageAdminUtils';
import type { UsageAdminDetailField, UsageAdminDetailGranularity } from './usageAdminUtils';
import { adminUsageTimeSeries, adminUsageWindow } from '../../../storybook/fixtures/adminUsage';

/** `window.Chart` (loaded via `.storybook/preview-head.html`) renders the canvas below. */
function Demo({ err, loading }: { err?: string; loading?: boolean }) {
  const [field, setField] = useState<UsageAdminDetailField>('requests');
  const [granularity, setGranularity] = useState<UsageAdminDetailGranularity>('hour');
  return (
    <UsageAdminTimeSeriesCard
      detailSeriesStart={adminUsageWindow.since}
      detailSeriesEnd={adminUsageWindow.until}
      detailSeries={adminUsageTimeSeries}
      detailSeriesErr={err || ''}
      detailSeriesLoading={!!loading}
      detailField={field}
      detailGranularity={granularity}
      fieldOptions={usageAdminFieldOptions}
      granularityOptions={usageAdminGranularityOptions}
      onFieldChange={setField}
      onGranularityChange={setGranularity}
    />
  );
}

const meta: Meta = {
  title: 'Pages/Admin/Usage/UsageAdminTimeSeriesCard',
  parameters: { layout: 'padded' },
};

export default meta;

export const Default: StoryObj = {
  render: () => <Demo />,
};

export const Loading: StoryObj = {
  render: () => <Demo loading />,
};

export const RequestError: StoryObj = {
  name: '请求出错',
  render: () => <Demo err="加载时间序列失败" />,
};
