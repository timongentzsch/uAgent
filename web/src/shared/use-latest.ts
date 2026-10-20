import { useRef } from "preact/hooks";

// A ref to the value of the latest render: what a callback that keeps its
// identity, or a listener added once, reads when it runs.
export function useLatest<T>(value: T) {
  const latest = useRef(value);
  latest.current = value;
  return latest;
}
