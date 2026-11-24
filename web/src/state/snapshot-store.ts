import type { Snapshot } from "../shared/types.ts";
import { valueStore } from "./value-store.ts";
export { useStoreValue as useSnapshots } from "./value-store.ts";

type Snapshots = Record<string, Snapshot>;

// The loaded conversations, outside component state: the event stream
// replaces them every frame, and only a component that reads one re-renders.
// Handlers read the current value with get() and subscribe to nothing.
export function snapshotStore() {
  const watched = new Map<string, number>();
  return {
    ...valueStore<Snapshots>({}),
    watching: () => [...watched.keys()],
    watches: (id: string) => watched.has(id),
    watch(id: string) {
      watched.set(id, (watched.get(id) || 0) + 1);
      return () => {
        const count = (watched.get(id) || 1) - 1;
        if (count) watched.set(id, count);
        else watched.delete(id);
      };
    },
  };
}
export type SnapshotStore = ReturnType<typeof snapshotStore>;
