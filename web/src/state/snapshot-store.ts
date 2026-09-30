import { useLayoutEffect, useReducer, useRef } from "preact/hooks";
import type { Snapshot } from "../shared/types.ts";

export type Snapshots = Record<string, Snapshot>;

// The loaded conversations, outside component state: the event stream
// replaces them every frame, and only a component that reads one re-renders.
// Handlers read the current value with get() and subscribe to nothing.
export function snapshotStore() {
  let value: Snapshots = {};
  const listeners = new Set<() => void>();
  return {
    get: () => value,
    set(next: Snapshots | ((prior: Snapshots) => Snapshots)) {
      const resolved = typeof next === "function" ? next(value) : next;
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
export type SnapshotStore = ReturnType<typeof snapshotStore>;

// What the caller selects from the store; it re-renders only when that
// changes identity, so a selector returns stored objects or primitives.
export function useSnapshots<T>(
  store: SnapshotStore,
  select: (snapshots: Snapshots) => T,
): T {
  const [, changed] = useReducer((count: number) => count + 1, 0);
  const value = select(store.get());
  const latest = useRef({ select, value });
  latest.current = { select, value };
  useLayoutEffect(() => {
    const check = () => {
      const { select, value } = latest.current;
      if (!Object.is(select(store.get()), value)) changed(0);
    };
    // A change between this render and the subscription still counts.
    check();
    return store.subscribe(check);
  }, [store]);
  return value;
}
