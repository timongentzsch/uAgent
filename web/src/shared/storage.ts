import { useEffect, useRef, useState } from "preact/hooks";

// Blocked site data can deny either the storage getter or its methods. Keep
// this visit's known values in memory so drafts and editors remain usable.
function memoryStorage(): Storage {
  const items = new Map<string, string>();
  return {
    get length() {
      return items.size;
    },
    key: (index) => [...items.keys()][index] ?? null,
    getItem: (key) => items.get(key) ?? null,
    setItem: (key, value) => void items.set(key, String(value)),
    removeItem: (key) => void items.delete(key),
    clear: () => items.clear(),
  };
}

export function safeStorage(get: () => Storage | undefined): Storage {
  const memory = memoryStorage();
  let native: Storage | undefined;
  try {
    native = get();
  } catch {
    // A denied getter uses memory from the first operation.
  }
  const read = <T>(operation: (storage: Storage) => T): T => {
    if (native) {
      try {
        return operation(native);
      } catch {
        native = undefined;
      }
    }
    return operation(memory);
  };
  const write = (operation: (storage: Storage) => void) => {
    operation(memory);
    if (native) {
      try {
        operation(native);
      } catch {
        // A full or denied store stays in memory for the rest of this visit.
        native = undefined;
      }
    }
  };
  return {
    get length() {
      return read((storage) => storage.length);
    },
    key: (index) => read((storage) => storage.key(index)),
    getItem: (key) => {
      const value = read((storage) => storage.getItem(key));
      // Successful reads still observe changes made by another tab. Remember
      // only what was read, rather than copying the whole store at startup.
      if (value === null) memory.removeItem(key);
      else memory.setItem(key, value);
      return value;
    },
    setItem: (key, value) => write((storage) => storage.setItem(key, value)),
    removeItem: (key) => write((storage) => storage.removeItem(key)),
    clear: () => write((storage) => storage.clear()),
  };
}

export const storage = safeStorage(() => globalThis.localStorage);
export const tabStorage = safeStorage(() => globalThis.sessionStorage);

// A value this browser keeps under `key`: read, written and watched in one
// place. A missing, corrupt or invalid value reads as `fallback` (by default,
// one of another shape is invalid), and writing the fallback forgets the key.
// `valid` makes the value of what was kept, and `kept` what is kept of a
// value; text is kept as itself, so public/theme.js reads the theme as
// written. With a `delay`, a write waits for changes to pause, and is made
// before the page may go away.
export function stored<T>(
  key: string,
  fallback: T,
  valid = (value: unknown) =>
    value !== null &&
    typeof value === typeof fallback &&
    Array.isArray(value) === Array.isArray(fallback)
      ? (value as T)
      : fallback,
  {
    where = storage,
    delay = 0,
    kept = (value) => value,
  }: { where?: Storage; delay?: number; kept?: (value: T) => unknown } = {},
) {
  const text = (value: T) => {
    const plain = kept(value);
    return typeof plain === "string" ? plain : JSON.stringify(plain);
  };
  const forgotten = text(fallback);
  const watchers = new Set<(value: T) => void>();
  let pending: { value: T } | undefined;
  let timer: ReturnType<typeof setTimeout> | undefined;
  const save = () => {
    clearTimeout(timer);
    if (!pending) return;
    const value = text(pending.value);
    pending = undefined;
    if (value === forgotten) where.removeItem(key);
    else where.setItem(key, value);
  };
  if (delay && globalThis.document) {
    addEventListener("pagehide", save);
    document.addEventListener(
      "visibilitychange",
      () => document.visibilityState === "hidden" && save(),
    );
  }
  return {
    fallback,
    read(): T {
      if (pending) return pending.value;
      const value = where.getItem(key);
      if (value === null) return fallback;
      try {
        return valid(JSON.parse(value));
      } catch {
        return valid(value);
      }
    },
    write(value: T) {
      pending = { value };
      clearTimeout(timer);
      if (delay) timer = setTimeout(save, delay);
      else save();
      for (const watcher of watchers) watcher(value);
    },
    subscribe(watcher: (value: T) => void) {
      watchers.add(watcher);
      return () => void watchers.delete(watcher);
    },
  };
}
export type Stored<T> = ReturnType<typeof stored<T>>;

// State kept in `store`: it starts from what is kept, is written whenever it
// changes, and follows writes made elsewhere on the page. What was read is
// not written again, so a page that changed nothing leaves what another tab
// kept alone.
export function useStored<T>(store: Stored<T>) {
  const [value, setValue] = useState(store.read);
  const kept = useRef(value);
  useEffect(
    () =>
      store.subscribe((next) => {
        // Its own write may trail newer state, which must not be undone.
        if (next === kept.current) return;
        kept.current = next;
        setValue(() => next);
      }),
    [store],
  );
  useEffect(() => {
    if (kept.current !== value) store.write((kept.current = value));
  }, [store, value]);
  return [value, setValue] as const;
}

// An editor's unsent work in this tab: a draft per item under `${key}-${id}`
// ("new" before the item has one), and the draft open when the tab last drew
// under `key`, which a reload reopens.
export function tabDrafts<T extends object>(key: string) {
  const draft = (id?: string) =>
    stored<T | null>(`${key}-${id || "new"}`, null, undefined, {
      where: tabStorage,
    });
  const open = stored<T | null>(key, null, undefined, { where: tabStorage });
  return {
    open,
    read: (id?: string) => draft(id).read(),
    keep(id: string | undefined, value: T) {
      draft(id).write(value);
      open.write(value);
    },
    forget: (id?: string) => draft(id).write(null),
  };
}
