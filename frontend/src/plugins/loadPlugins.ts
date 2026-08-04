// v3 plugin frontend loader. The core serves the plugin package snapshot's
// conventional frontend/entry.js files at /api/plugins/frontend; each entry is
// a plain ESM module that mounts itself by side effect. There is no frontend
// SDK and no call ABI: the core only dynamic-imports the entry and never reads
// its exports. A failing entry blocks the console (single-plugin isolation is
// deliberately absent, per the v3 package contract).

type PluginFrontendEntry = {
  id: string;
  url: string;
};

export async function loadPluginFrontends(): Promise<void> {
  let entries: PluginFrontendEntry[];
  try {
    const response = await fetch('/api/plugins/frontend', { headers: { Accept: 'application/json' } });
    if (!response.ok) {
      throw new Error(`plugins/frontend returned ${response.status}`);
    }
    const payload = (await response.json()) as { success?: boolean; data?: PluginFrontendEntry[] };
    entries = Array.isArray(payload.data) ? payload.data : [];
  } catch (error) {
    console.error('[plugins] failed to list frontend entries:', error);
    throw error;
  }

  for (const entry of entries) {
    try {
      // Side-effect import: the module mounts itself.
      await import(/* @vite-ignore */ entry.url);
    } catch (error) {
      console.error(`[plugins] failed to load frontend entry ${entry.id}:`, error);
      throw error;
    }
  }
}
