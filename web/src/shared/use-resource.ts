import { useEffect, useState, type Inputs } from "preact/hooks";

// Loads a value whenever `deps` change or `retry` asks again: the error
// clears as a load starts, and a load a newer one replaced lands nowhere.
// The last value (first `initial()`) stays until the next arrives; `load`
// returns nothing to skip a round (offline, say). `setValue` and
// `setError` let the owner apply its own writes' answers. `load` gets a
// signal that aborts once a newer load replaced it, for its requests and
// for a load that writes state of its own before it returns.
export function useResource<T>(
  load: (signal: AbortSignal) => Promise<T> | undefined,
  deps: Inputs,
  initial?: () => T | undefined,
) {
  const [value, setValue] = useState<T | undefined>(initial);
  const [error, setError] = useState<unknown>(null);
  const [attempt, setAttempt] = useState(0);
  // biome-ignore lint/correctness/useExhaustiveDependencies: the caller's deps say when to load
  useEffect(() => {
    const abort = new AbortController();
    const { signal } = abort;
    setError(null);
    load(signal)?.then(
      (next) => signal.aborted || setValue(() => next),
      (failure) => signal.aborted || setError(failure),
    );
    return () => abort.abort();
  }, [...deps, attempt]);
  return {
    value,
    error,
    retry: () => setAttempt((prior) => prior + 1),
    setValue,
    setError,
  };
}
