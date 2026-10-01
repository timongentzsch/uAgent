import { useState } from "preact/hooks";

// One action at a time with its outcome: `run` resolves true when the work
// succeeded, `busy` is true while it runs, and `error` holds the last
// failure until the next run starts.
export function useAction() {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const run = async (work: () => Promise<unknown>) => {
    setBusy(true);
    setError(null);
    try {
      await work();
      return true;
    } catch (failure) {
      setError(failure);
      return false;
    } finally {
      setBusy(false);
    }
  };
  return { run, busy, error };
}
