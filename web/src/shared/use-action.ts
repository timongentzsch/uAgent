import { useRef, useState } from "preact/hooks";

// One action at a time with its outcome: `run` resolves true when the work
// succeeded, `busy` is true while it runs, and `error` holds the last
// failure until the next run starts (or `setError` sets or clears it).
// Overlapping calls resolve false without starting work, even before render.
export function useAction(report?: (failure: unknown) => void) {
  const running = useRef(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const run = async (work: () => Promise<unknown>) => {
    if (running.current) return false;
    running.current = true;
    setBusy(true);
    setError(null);
    try {
      await work();
      return true;
    } catch (failure) {
      setError(failure);
      report?.(failure);
      return false;
    } finally {
      running.current = false;
      setBusy(false);
    }
  };
  return { run, busy, error, setError };
}
