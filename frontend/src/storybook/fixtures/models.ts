import type { PluginModel } from '../../api/models';

export const pluginModels: PluginModel[] = [
  { id: 'claude-opus-5', name: 'Claude Opus 5', pricing: { input: '0.015', output: '0.075', unit: 'USD/1K tokens' } },
  {
    id: 'claude-sonnet-5',
    name: 'Claude Sonnet 5',
    pricing: { input: '0.003', output: '0.015', unit: 'USD/1K tokens' },
  },
  { id: 'claude-haiku-4-5', pricing: { input: '0.0008', output: '0.004', unit: 'USD/1K tokens' } },
];
