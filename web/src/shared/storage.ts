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
