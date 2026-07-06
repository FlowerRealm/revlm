import { useCallback, useMemo } from 'react';
import { useSearchParams } from 'react-router-dom';

export function useAdminSelectionParam(paramName: string) {
  const [searchParams, setSearchParams] = useSearchParams();

  const value = useMemo(() => searchParams.get(paramName) || '', [paramName, searchParams]);

  const setValue = useCallback(
    (nextValue: string | number | null | undefined, opts?: { replace?: boolean }) => {
      const next = new URLSearchParams(searchParams);
      const normalized = nextValue === null || nextValue === undefined ? '' : String(nextValue).trim();
      if (normalized) next.set(paramName, normalized);
      else next.delete(paramName);
      setSearchParams(next, { replace: opts?.replace !== false });
    },
    [paramName, searchParams, setSearchParams]
  );

  return [value, setValue] as const;
}
