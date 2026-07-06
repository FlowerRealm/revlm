import { useEffect, useRef, useState } from 'react';

export function usePresence(open: boolean, durationMs: number) {
  const [present, setPresent] = useState(open);
  const [phase, setPhase] = useState<'enter' | 'leave'>(open ? 'enter' : 'leave');
  const timerRef = useRef<number | null>(null);
  const frameRef = useRef<number | null>(null);

  useEffect(() => {
    if (timerRef.current !== null) {
      window.clearTimeout(timerRef.current);
      timerRef.current = null;
    }
    if (frameRef.current !== null) {
      window.cancelAnimationFrame(frameRef.current);
      frameRef.current = null;
    }

    if (open) {
      frameRef.current = window.requestAnimationFrame(() => {
        setPresent(true);
        setPhase('enter');
        frameRef.current = null;
      });
      return () => {
        if (frameRef.current !== null) {
          window.cancelAnimationFrame(frameRef.current);
          frameRef.current = null;
        }
      };
    }

    frameRef.current = window.requestAnimationFrame(() => {
      setPhase('leave');
      frameRef.current = null;
    });
    timerRef.current = window.setTimeout(
      () => {
        setPresent(false);
        timerRef.current = null;
      },
      Math.max(0, durationMs)
    );

    return () => {
      if (frameRef.current !== null) {
        window.cancelAnimationFrame(frameRef.current);
        frameRef.current = null;
      }
      if (timerRef.current !== null) {
        window.clearTimeout(timerRef.current);
        timerRef.current = null;
      }
    };
  }, [durationMs, open]);

  return { present, phase };
}
