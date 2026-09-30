import { useEffect, useState, type Inputs } from "preact/hooks";

// Loads a value whenever `deps` change or `retry` asks again: the error
// clears as a load starts, and a load a newer one replaced lands nowhere.
// The last value (first `initial()`) stays until the next arrives; `load`
// returns nothing to skip a round (offline, say). `setValue` and
// `setError` let the owner apply its own writes' answers.
export function useResource<T>(
  load: () => Promise<T> | undefined,
  deps: Inputs,
  initial?: () => T | undefined,
) {
  const [value, setValue] = useState<T | undefined>(initial);
  const [error, setError] = useState<unknown>(null);
  const [attempt, setAttempt] = useState(0);
  useEffect(() => {
    let active = true;
    setError(null);
    load()?.then(
      (next) => active && setValue(() => next),
      (failure) => active && setError(failure),
    );
    return () => {
      active = false;
    };
  }, [...deps, attempt]);
  return {
    value,
    error,
    retry: () => setAttempt((prior) => prior + 1),
    setValue,
    setError,
  };
}
