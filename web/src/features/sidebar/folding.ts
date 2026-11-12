import { useEffect, useState } from "preact/hooks";
import { stored } from "../../shared/storage.ts";
import { storageKeys } from "../../shared/storage-keys.ts";

// One set per name, shared: Alt+↑/↓ reads the same set the list draws from.
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

export const collapsed = remembered("collapsed");
export const opened = remembered("opened");
// A fold given `closed` is folded until opened.
export const folded = (name: string, closed = false) =>
  closed ? !opened.has(name) : collapsed.has(name);
export const placeKey = (host: string, path: string) =>
  host ? `${host}:${path}` : path;

// Redraws the caller when the set changes.
export function useFolding() {
  const [, redraw] = useState(0);
  useEffect(() => {
    const changed = () => redraw((count) => count + 1);
    const a = collapsed.subscribe(changed),
      b = opened.subscribe(changed);
    return () => {
      a();
      b();
    };
  }, []);
}
