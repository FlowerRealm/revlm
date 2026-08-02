type FrontendPluginEntry = {
  id: string;
  url: string;
};

type FrontendPluginResponse = {
  success?: boolean;
  data?: FrontendPluginEntry[];
};

// There is intentionally no frontend SDK. A trusted plugin receives a normal
// browser module slot and can replace the page, mount its own React tree, or
// patch anything else it can reach.
export async function loadPluginFrontends() {
  try {
    const response = await fetch('/api/plugins/frontend', { credentials: 'same-origin' });
    if (!response.ok) return;
    const payload = (await response.json()) as FrontendPluginResponse;
    if (!payload.success || !Array.isArray(payload.data)) return;
    for (const entry of payload.data) {
      if (!entry || typeof entry.url !== 'string' || !entry.url.startsWith('/api/plugins/frontend/')) continue;
      await import(/* @vite-ignore */ entry.url);
    }
  } catch (error) {
    // A bad optional UI module should not prevent the control plane from
    // opening. Its backend replacement still follows the normal preload path.
    console.error('failed to load plugin frontend', error);
  }
}
