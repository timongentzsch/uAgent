import { useEffect, useState, type Inputs } from "preact/hooks";

// Loads whenever `deps` change or `retry` asks. The last value stays until
// the next arrives; `load` returns nothing to skip a round. Its signal aborts
// once a newer load replaced it, and that load's result lands nowhere.
export function useResource<T>(
  load: (signal: AbortSignal) => Promise<T> | undefined,
  deps: Inputs,
  initial?: () => T | undefined,
  delay = 0,
) {
  const [value, setValue] = useState<T | undefined>(initial);
  const [error, setError] = useState<unknown>(null);
  const [attempt, setAttempt] = useState(0);
  // biome-ignore lint/correctness/useExhaustiveDependencies: the caller's deps say when to load
  useEffect(() => {
    const abort = new AbortController();
    const { signal } = abort;
    setError(null);
    const start = () =>
      load(signal)?.then(
        (next) => signal.aborted || setValue(() => next),
        (failure) => signal.aborted || setError(failure),
      );
    const timer = delay ? setTimeout(start, delay) : undefined;
    if (!delay) start();
    return () => {
      clearTimeout(timer);
      abort.abort();
    };
  }, [...deps, attempt, delay]);
  return {
    value,
    error,
    retry: () => setAttempt((prior) => prior + 1),
    setValue,
    setError,
  };
}
