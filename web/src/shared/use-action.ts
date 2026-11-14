import { useRef, useState } from "preact/hooks";

// `run` resolves true when the work succeeded; `error` holds the last
// failure until the next run starts. Overlap resolves false without starting
// work, unless `concurrent`; busy then covers every pending call.
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
