import { useState } from 'react';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { UsageTimeSeriesCard } from './UsageTimeSeriesCard';
import type { UsageTimeSeriesField, UsageTimeSeriesGranularity } from './UsageTimeSeriesChartCard';
import { usageTimeSeries, usageWindow } from '../../storybook/fixtures/usage';

const fieldOptions: Array<{ value: UsageTimeSeriesField; label: string }> = [
  { value: 'requests', label: '请求数' },
  { value: 'usd', label: '消耗 (USD)' },
  { value: 'avg_first_token_latency', label: '首字延迟 (s)' },
];
const granularityOptions: Array<{ value: UsageTimeSeriesGranularity; label: string }> = [
  { value: 'hour', label: '按小时' },
  { value: 'day', label: '按天' },
];

/** `window.Chart` (loaded via `.storybook/preview-head.html`) renders the canvas below. */
function Demo({ err, loading }: { err?: string; loading?: boolean }) {
  const [field, setField] = useState<UsageTimeSeriesField>('requests');
  const [granularity, setGranularity] = useState<UsageTimeSeriesGranularity>('hour');
  return (
    <UsageTimeSeriesCard
      rangeSinceText={usageWindow.since}
      rangeUntilText={usageWindow.until}
      detailSeries={usageTimeSeries}
      detailSeriesErr={err || ''}
      detailSeriesLoading={!!loading}
      detailField={field}
      setDetailField={setField}
      detailGranularity={granularity}
      setDetailGranularity={setGranularity}
      fieldOptions={fieldOptions}
      granularityOptions={granularityOptions}
    />
  );
}

const meta: Meta = {
  title: 'Pages/Usage/UsageTimeSeriesCard',
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
