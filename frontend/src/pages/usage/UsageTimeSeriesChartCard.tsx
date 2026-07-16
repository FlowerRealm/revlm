import { useEffect, useRef, type MutableRefObject } from 'react';

import { formatIntComma } from '../../format/int';

export type UsageTimeSeriesChartPoint = {
  bucket: string;
  requests: number;
  tokens: number;
  usd: number;
  cache_ratio: number;
  avg_first_token_latency: number;
  tokens_per_second: number;
};

export type UsageTimeSeriesField =
  'usd' | 'requests' | 'tokens' | 'cache_ratio' | 'avg_first_token_latency' | 'tokens_per_second';

export type UsageTimeSeriesGranularity = 'hour' | 'day';

type ChartInstance = {
  destroy?: () => void;
};

type ChartConstructor = new (ctx: CanvasRenderingContext2D, config: unknown) => ChartInstance;
type Option<T> = { value: T; label: string };

type Props<TPoint extends UsageTimeSeriesChartPoint> = {
  title: string;
  chartTitle: string;
  rangeSinceText: string;
  rangeUntilText: string;
  detailSeries: TPoint[];
  detailSeriesErr: string;
  detailSeriesLoading: boolean;
  detailField: UsageTimeSeriesField;
  detailGranularity: UsageTimeSeriesGranularity;
  fieldOptions: Array<Option<UsageTimeSeriesField>>;
  granularityOptions: Array<Option<UsageTimeSeriesGranularity>>;
  onFieldChange: (value: UsageTimeSeriesField) => void;
  onGranularityChange: (value: UsageTimeSeriesGranularity) => void;
};

export function UsageTimeSeriesChartCard<TPoint extends UsageTimeSeriesChartPoint>({
  title,
  chartTitle,
  rangeSinceText,
  rangeUntilText,
  detailSeries,
  detailSeriesErr,
  detailSeriesLoading,
  detailField,
  detailGranularity,
  fieldOptions,
  granularityOptions,
  onFieldChange,
  onGranularityChange,
}: Props<TPoint>) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const chartRef = useRef<ChartInstance | null>(null);

  useEffect(() => {
    const destroy = destroyChart(chartRef);
    destroy();
    const ChartCtor = getChartConstructor();
    const canvas = canvasRef.current;
    if (!ChartCtor || !canvas) return destroy;

    const ctx = canvas.getContext('2d');
    if (!ctx) return destroy;

    chartRef.current = new ChartCtor(
      ctx,
      createChartConfig(canvas, chartTitle, detailSeries, detailField, detailGranularity)
    );

    return destroy;
  }, [chartTitle, detailSeries, detailField, detailGranularity]);

  return (
    <div className="card border-0 p-0 overflow-hidden">
      <div className="card-header bg-white py-3 border-bottom px-4">
        <h5 className="mb-0 fw-bold">
          <i className="ri-line-chart-line me-2"></i>
          {title}
        </h5>
      </div>
      <div className="card-body p-4">
        <div className="d-flex flex-wrap align-items-center gap-3 mb-2">
          <div className="d-flex align-items-center gap-2 flex-grow-1">
            <div className="d-flex flex-wrap gap-1">
              {fieldOptions.map((option) => (
                <button
                  key={option.value}
                  type="button"
                  className={`btn btn-sm ${detailField === option.value ? 'btn-primary' : 'btn-outline-secondary'}`}
                  onClick={() => onFieldChange(option.value)}
                >
                  {option.label}
                </button>
              ))}
            </div>
          </div>
          <div className="d-flex align-items-center gap-2 ms-auto">
            <div className="d-flex gap-1">
              {granularityOptions.map((option) => (
                <button
                  key={option.value}
                  type="button"
                  className={`btn btn-sm ${detailGranularity === option.value ? 'btn-primary' : 'btn-outline-secondary'}`}
                  onClick={() => onGranularityChange(option.value)}
                >
                  {option.label}
                </button>
              ))}
            </div>
          </div>
        </div>

        <div className="small text-muted mb-2">
          时间区间：{rangeSinceText || '-'} ~ {rangeUntilText || '-'}
        </div>
        {detailSeriesErr ? <div className="alert alert-danger py-2 mb-2">{detailSeriesErr}</div> : null}
        {detailSeriesLoading ? (
          <div className="text-muted small py-4">时间序列加载中…</div>
        ) : (
          <div style={{ height: 280 }}>
            <canvas ref={canvasRef}></canvas>
          </div>
        )}
      </div>
    </div>
  );
}

function getChartConstructor() {
  return (globalThis.window as unknown as { Chart?: ChartConstructor })?.Chart;
}

function destroyChart(ref: MutableRefObject<ChartInstance | null>) {
  return () => {
    try {
      ref.current?.destroy?.();
    } catch {
      // ignore
    }
    ref.current = null;
  };
}

function createChartConfig<TPoint extends UsageTimeSeriesChartPoint>(
  canvas: HTMLCanvasElement,
  chartTitle: string,
  detailSeries: TPoint[],
  detailField: UsageTimeSeriesField,
  detailGranularity: UsageTimeSeriesGranularity
) {
  const palette = readPalette(canvas);
  const meta = createFieldMeta(palette)[detailField];

  return {
    type: 'line',
    data: {
      labels: detailSeries.map((point) => point.bucket),
      datasets: [
        {
          label: meta.label,
          data: detailSeries.map((point) => meta.read(point)),
          borderColor: meta.color,
          backgroundColor: meta.color.replace('0.95', '0.18'),
          pointRadius: 2,
          tension: 0.2,
        },
      ],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      interaction: { mode: 'index', intersect: false },
      plugins: {
        legend: { position: 'bottom' },
        title: { display: true, text: chartTitle },
      },
      scales: {
        x: {
          grid: { display: false },
          ticks: {
            autoSkip: true,
            maxTicksLimit: detailGranularity === 'hour' ? 10 : 14,
            maxRotation: 0,
            minRotation: 0,
          },
        },
        y: {
          beginAtZero: true,
          suggestedMax: detailField === 'cache_ratio' ? 100 : undefined,
          grid: { color: palette.grid },
          ...(detailField === 'requests' || detailField === 'tokens'
            ? {
                ticks: {
                  callback: (value: string | number) => formatIntComma(value),
                },
              }
            : {}),
        },
      },
    },
  };
}

function readPalette(canvas: HTMLCanvasElement) {
  const css = getComputedStyle(canvas);
  const rgb = (varName: string, fallback: string) => (css.getPropertyValue(varName).trim() || fallback).trim();
  const color = (rgbValue: string, alpha: number) => `rgba(${rgbValue}, ${alpha})`;

  return {
    info: color(rgb('--bs-info-rgb', '53, 90, 96'), 0.95),
    success: color(rgb('--bs-success-rgb', '47, 107, 75'), 0.95),
    warning: color(rgb('--bs-warning-rgb', '122, 98, 50'), 0.95),
    danger: color(rgb('--bs-danger-rgb', '122, 52, 52'), 0.95),
    primary: color(rgb('--bs-primary-rgb', '60, 138, 97'), 0.95),
    secondary: color(rgb('--bs-secondary-rgb', '99, 116, 107'), 0.95),
    grid: color(rgb('--bs-secondary-rgb', '99, 116, 107'), 0.18),
  };
}

function createFieldMeta(palette: ReturnType<typeof readPalette>) {
  return {
    usd: {
      label: '消耗 (USD)',
      color: palette.primary,
      read: (point: UsageTimeSeriesChartPoint) => point.usd,
    },
    requests: {
      label: '请求数',
      color: palette.info,
      read: (point: UsageTimeSeriesChartPoint) => point.requests,
    },
    tokens: {
      label: 'Token',
      color: palette.success,
      read: (point: UsageTimeSeriesChartPoint) => point.tokens,
    },
    cache_ratio: {
      label: '缓存率 (%)',
      color: palette.warning,
      read: (point: UsageTimeSeriesChartPoint) => point.cache_ratio,
    },
    avg_first_token_latency: {
      label: '首字延迟 (s)',
      color: palette.danger,
      read: (point: UsageTimeSeriesChartPoint) => point.avg_first_token_latency / 1000,
    },
    tokens_per_second: {
      label: 'Tokens/s',
      color: palette.secondary,
      read: (point: UsageTimeSeriesChartPoint) => point.tokens_per_second,
    },
  } satisfies Record<
    UsageTimeSeriesField,
    {
      label: string;
      color: string;
      read: (point: UsageTimeSeriesChartPoint) => number;
    }
  >;
}
