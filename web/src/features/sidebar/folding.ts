import { useEffect, useState } from "preact/hooks";
import { stored } from "../../shared/storage.ts";
import { storageKeys } from "../../shared/storage-keys.ts";

// A set of places this browser remembers, by their path: the list's
// Alt+↑/↓ steps through what is shown, so it reads the same set the list
// draws from.
function remembered(name: string) {
  const kept = stored<string[]>(storageKeys.places(name), []);
  const paths = new Set(kept.read());
  return {
    has: (path: string) => paths.has(path),
    set(path: string, on: boolean) {
      if (paths.has(path) === on) return;
      if (on) paths.add(path);
      else paths.delete(path);
      kept.write([...paths]);
    },
    subscribe: kept.subscribe,
  };
}

// Places folded to their header.
export const collapsed = remembered("collapsed");
// Conversations whose finished agents are unfolded, by id.
export const unfolded = remembered("agents");

// Redraws the caller when either set changes.
export function useFolding() {
  const [, redraw] = useState(0);
  useEffect(() => {
    const changed = () => redraw((count) => count + 1);
    const stops = [collapsed.subscribe(changed), unfolded.subscribe(changed)];
    return () => stops.forEach((stop) => stop());
  }, []);
}
