import { useLayoutEffect, useReducer } from "preact/hooks";
import { useLatest } from "../shared/use-latest.ts";

// Synchronous reads for actions, fine-grained subscriptions for visible state.
export function valueStore<T extends object>(initial: T) {
  let value = initial;
  const listeners = new Set<() => void>();
  return {
    get: () => value,
    set(next: T | ((prior: T) => T)) {
      const resolved =
        typeof next === "function" ? (next as (prior: T) => T)(value) : next;
      if (resolved === value) return;
      value = resolved;
      listeners.forEach((listener) => listener());
    },
    subscribe(listener: () => void) {
      listeners.add(listener);
      return () => void listeners.delete(listener);
    },
  };
}

export type ValueStore<T extends object> = ReturnType<typeof valueStore<T>>;

// What the caller selects from the store; it re-renders only when that
// changes identity, so a selector returns stored objects or primitives.
export function useStoreValue<State extends object, T>(
  store: ValueStore<State>,
  select: (state: State) => T,
  active = true,
): T {
  const [, changed] = useReducer((count: number) => count + 1, 0);
  const value = select(store.get());
  const latest = useLatest({ select, value });
  useLayoutEffect(() => {
    if (!active) return;
    const check = () => {
      const { select, value } = latest.current;
      if (!Object.is(select(store.get()), value)) changed(0);
    };
    // A change between this render and the subscription still counts.
    check();
    return store.subscribe(check);
  }, [store, active]);
  return value;
}
