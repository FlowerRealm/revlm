import type { AdminUsageTimeSeriesPoint } from '../../../api/admin/usage';
import { UsageTimeSeriesChartCard } from '../../usage/UsageTimeSeriesChartCard';
import type { UsageAdminDetailField, UsageAdminDetailGranularity } from './usageAdminUtils';

type Option<T> = { value: T; label: string };

type Props = {
  detailSeries: AdminUsageTimeSeriesPoint[];
  detailSeriesStart: string;
  detailSeriesEnd: string;
  detailSeriesErr: string;
  detailSeriesLoading: boolean;
  detailField: UsageAdminDetailField;
  detailGranularity: UsageAdminDetailGranularity;
  fieldOptions: Array<Option<UsageAdminDetailField>>;
  granularityOptions: Array<Option<UsageAdminDetailGranularity>>;
  onFieldChange: (value: UsageAdminDetailField) => void;
  onGranularityChange: (value: UsageAdminDetailGranularity) => void;
};

export function UsageAdminTimeSeriesCard({
  detailSeries,
  detailSeriesStart,
  detailSeriesEnd,
  detailSeriesErr,
  detailSeriesLoading,
  detailField,
  detailGranularity,
  fieldOptions,
  granularityOptions,
  onFieldChange,
  onGranularityChange,
}: Props) {
  return (
    <UsageTimeSeriesChartCard
      title="全站时间序列"
      chartTitle="全站用量 · 时间序列"
      rangeSinceText={detailSeriesStart}
      rangeUntilText={detailSeriesEnd}
      detailSeries={detailSeries}
      detailSeriesErr={detailSeriesErr}
      detailSeriesLoading={detailSeriesLoading}
      detailField={detailField}
      detailGranularity={detailGranularity}
      fieldOptions={fieldOptions}
      granularityOptions={granularityOptions}
      onFieldChange={onFieldChange}
      onGranularityChange={onGranularityChange}
    />
  );
}
