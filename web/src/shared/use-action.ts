import { useRef, useState } from "preact/hooks";

// An action with its outcome: `run` resolves true when the work
// succeeded, `busy` is true while it runs, and `error` holds the last
// failure until the next run starts (or `setError` sets or clears it).
// By default, overlap resolves false without starting work, even before render.
// Independent reads can opt into concurrency; busy then covers every pending call.
export function useAction({
  concurrent = false,
  report,
}: {
  concurrent?: boolean;
  report?: (failure: unknown) => void;
} = {}) {
  const running = useRef(0);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const run = async (work: () => Promise<unknown>) => {
    if (running.current && !concurrent) return false;
    running.current++;
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
      setBusy(--running.current > 0);
    }
  };
  return { run, busy, error, setError };
}
