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

// Redraws the caller when the set changes.
export function useFolding() {
  const [, redraw] = useState(0);
  useEffect(() => collapsed.subscribe(() => redraw((count) => count + 1)), []);
}
