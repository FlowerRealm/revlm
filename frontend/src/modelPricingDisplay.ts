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

export type CacheUsagePricing = {
  input_tokens_cache_read: number;
  input_tokens_cache_creation: number;
  input_tokens_cache_creation_5m: number;
  input_tokens_cache_creation_1h: number;
  cache_read_usd_per_1m: string;
  cache_creation_5m_usd_per_1m: string;
  cache_creation_1h_usd_per_1m: string;
};

export function providerCacheUsageRows(owner: string | null | undefined, pricing: CacheUsagePricing) {
  if (isAnthropicOwner(owner)) {
    const rows = [
      {
        key: 'cache_read',
        label: '缓存读取',
        tokens: pricing.input_tokens_cache_read || 0,
        price: pricing.cache_read_usd_per_1m || '0',
      },
    ];
    // 仅当存在 1h 写入时才拆成两行，否则保持单行“缓存创建”（兼容历史数据）。
    if ((pricing.input_tokens_cache_creation_1h || 0) > 0) {
      rows.push({
        key: 'cache_creation_5m',
        label: '缓存创建·5m',
        tokens: pricing.input_tokens_cache_creation_5m || 0,
        price: pricing.cache_creation_5m_usd_per_1m || '0',
      });
      rows.push({
        key: 'cache_creation_1h',
        label: '缓存创建·1h',
        tokens: pricing.input_tokens_cache_creation_1h || 0,
        price: pricing.cache_creation_1h_usd_per_1m || '0',
      });
    } else {
      rows.push({
        key: 'cache_creation',
        label: '缓存创建',
        tokens: pricing.input_tokens_cache_creation || 0,
        price: pricing.cache_creation_5m_usd_per_1m || '0',
      });
    }
    return rows;
  }
  return [
    {
      key: 'cache_input',
      label: '缓存输入',
      tokens: pricing.input_tokens_cache_read || 0,
      price: pricing.cache_read_usd_per_1m || '0',
    },
  ];
}
