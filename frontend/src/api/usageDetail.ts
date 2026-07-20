export type UsageEventDetail = {
  event_id: number;
  pricing_breakdown?: UsageEventPricingBreakdown;
};

export type UsageEventPricingBreakdown = {
  model_public_id?: string | null;
  model_found: boolean;
  owned_by?: string | null;
  service_tier?: string | null;
  pricing_kind?: string;

  input_tokens_total: number;
  input_tokens_cache_read: number;
  input_tokens_cache_creation: number;
  input_tokens_cache_creation_5m: number;
  input_tokens_cache_creation_1h: number;
  input_tokens_billable: number;
  output_tokens_total: number;

  input_usd_per_1m: string;
  output_usd_per_1m: string;
  cache_read_usd_per_1m: string;
  cache_creation_5m_usd_per_1m: string;
  cache_creation_1h_usd_per_1m: string;

  input_cost_usd: string;
  output_cost_usd: string;
  cache_read_cost_usd: string;
  cache_creation_cost_usd: string;
  cache_creation_5m_cost_usd: string;
  cache_creation_1h_cost_usd: string;
  base_cost_usd: string;

  tier_multiplier: number;
  channel_multiplier: number;
  final_cost_usd: string;
};
