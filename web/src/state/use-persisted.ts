import { useEffect, useRef } from "preact/hooks";
import { writeStored } from "./store.ts";

// Keeps a value in browser storage whenever it changes: at once, or with a
// `delay` once changes pause and before the page may go away. The value as
// it was read, or as last kept, is not written again, so a page that
// changed nothing leaves what another tab kept alone. `kept` is what of the
// value is written.
export function usePersisted<T>(
  store: Pick<Storage, "setItem">,
  key: string,
  value: T,
  kept: (value: T) => unknown = (value) => value,
  delay = 0,
) {
  const saved = useRef(value);
  // biome-ignore lint/correctness/useExhaustiveDependencies: where and how a value is kept is fixed for each caller
  useEffect(() => {
    const save = () => {
      if (saved.current === value) return;
      saved.current = value;
      writeStored(store, key, kept(value));
    };
    if (!delay) return save();
    const timer = setTimeout(save, delay);
    const hidden = () => document.visibilityState === "hidden" && save();
    addEventListener("pagehide", save);
    document.addEventListener("visibilitychange", hidden);
    return () => {
      clearTimeout(timer);
      removeEventListener("pagehide", save);
      document.removeEventListener("visibilitychange", hidden);
    };
  }, [value]);
}
