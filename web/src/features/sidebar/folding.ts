import { useEffect, useState } from "preact/hooks";
import { storage } from "../../shared/storage.ts";
import { readStored, writeStored } from "../../state/store.ts";

const watchers = new Set<() => void>();

// A set of places this browser remembers, by their path: the list's
// Alt+↑/↓ steps through what is shown, so it reads the same set the list
// draws from.
function remembered(name: string) {
  const key = `uagent.sidebar.${name}`;
  const paths = new Set(readStored<string[]>(storage, key, []));
  return {
    has: (path: string) => paths.has(path),
    set(path: string, on: boolean) {
      if (paths.has(path) === on) return;
      if (on) paths.add(path);
      else paths.delete(path);
      writeStored(storage, key, [...paths]);
      for (const redraw of watchers) redraw();
    },
  };
}

// Places folded to their header, and panels showing who is in them.
export const collapsed = remembered("collapsed");
export const panelsOpen = remembered("panels");

// Redraws the caller when either set changes.
export function useFolding() {
  const [, redraw] = useState(0);
  useEffect(() => {
    const watcher = () => redraw((count) => count + 1);
    watchers.add(watcher);
    return () => void watchers.delete(watcher);
  }, []);
}
