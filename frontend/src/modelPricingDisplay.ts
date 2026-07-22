export type ModelOwner = 'openai' | 'anthropic';

export function modelOwner(raw?: string | null): ModelOwner {
  return String(raw || '')
    .trim()
    .toLowerCase() === 'anthropic'
    ? 'anthropic'
    : 'openai';
}

export function isAnthropicOwner(raw?: string | null) {
  return modelOwner(raw) === 'anthropic';
}

export function providerCachePriceRows<
  T extends {
    cache_read_input_usd_per_1m: string;
    cache_creation_input_usd_per_1m: string;
    cache_creation_1h_input_usd_per_1m: string;
  },
>(owner: string | null | undefined, prices: T) {
  if (isAnthropicOwner(owner)) {
    return [
      {
        key: 'cache_read_input_usd_per_1m',
        label: '缓存读取',
        shortLabel: 'Cache Read',
        value: prices.cache_read_input_usd_per_1m,
      },
      {
        key: 'cache_creation_input_usd_per_1m',
        label: '缓存创建·5m',
        shortLabel: 'Cache 5m',
        value: prices.cache_creation_input_usd_per_1m,
      },
      {
        key: 'cache_creation_1h_input_usd_per_1m',
        label: '缓存创建·1h',
        shortLabel: 'Cache 1h',
        value: prices.cache_creation_1h_input_usd_per_1m,
      },
    ];
  }
  return [
    {
      key: 'cache_read_input_usd_per_1m',
      label: '缓存输入',
      shortLabel: 'Cache In',
      value: prices.cache_read_input_usd_per_1m,
    },
  ];
}
