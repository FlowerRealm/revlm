import type { UsageTimeSeriesPoint } from '../../api/usage';
import {
  UsageTimeSeriesChartCard,
  type UsageTimeSeriesField,
  type UsageTimeSeriesGranularity,
} from './UsageTimeSeriesChartCard';

type FieldOption = { value: UsageTimeSeriesField; label: string };
type GranularityOption = { value: UsageTimeSeriesGranularity; label: string };

export function UsageTimeSeriesCard({
  rangeSinceText,
  rangeUntilText,
  detailSeries,
  detailSeriesErr,
  detailSeriesLoading,
  detailField,
  setDetailField,
  detailGranularity,
  setDetailGranularity,
  fieldOptions,
  granularityOptions,
}: {
  rangeSinceText: string;
  rangeUntilText: string;
  detailSeries: UsageTimeSeriesPoint[];
  detailSeriesErr: string;
  detailSeriesLoading: boolean;
  detailField: UsageTimeSeriesField;
  setDetailField: (value: UsageTimeSeriesField) => void;
  detailGranularity: UsageTimeSeriesGranularity;
  setDetailGranularity: (value: UsageTimeSeriesGranularity) => void;
  fieldOptions: FieldOption[];
  granularityOptions: GranularityOption[];
}) {
  return (
    <UsageTimeSeriesChartCard
      title="用量时间序列"
      chartTitle="用量时间序列"
      rangeSinceText={rangeSinceText}
      rangeUntilText={rangeUntilText}
      detailSeries={detailSeries}
      detailSeriesErr={detailSeriesErr}
      detailSeriesLoading={detailSeriesLoading}
      detailField={detailField}
      detailGranularity={detailGranularity}
      fieldOptions={fieldOptions}
      granularityOptions={granularityOptions}
      onFieldChange={setDetailField}
      onGranularityChange={setDetailGranularity}
    />
  );
}
