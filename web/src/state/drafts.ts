import { deviceStorage, scheduleSave } from "../shared/storage.ts";
import { storageKeys } from "../shared/storage-keys.ts";
import { draftSaveDelayMs } from "../shared/limits.ts";
import type { Draft } from "../shared/types.ts";
import { hasContent } from "./store.ts";
import { valueStore } from "./value-store.ts";

type Drafts = Record<string, Draft>;
const prefix = storageKeys.draftPrefix;
const key = (id: string) => prefix + id;
const keys = () =>
  Array.from({ length: deviceStorage.length }, (_, at) =>
    deviceStorage.key(at),
  ).filter((name): name is string => !!name?.startsWith(prefix));

function restored(value: unknown): Draft | undefined {
  const draft = value as Draft | undefined;
  if (
    !draft ||
    typeof draft.text !== "string" ||
    !Array.isArray(draft.files) ||
    !Array.isArray(draft.quotes)
  )
    return;
  // An interrupted upload never becomes an attachment on reload.
  const clean = {
    ...draft,
    files: draft.files.filter((file) => file && !file.pending),
  };
  return hasContent(clean) ? clean : undefined;
}

function read(name: string) {
  try {
    return restored(JSON.parse(deviceStorage.getItem(name) ?? "null"));
  } catch {
    return undefined;
  }
}

function replace(drafts: Drafts, id: string, value?: Draft) {
  const next = { ...drafts };
  if (value) next[id] = value;
  else delete next[id];
  return next;
}

// Each tab writes only conversations it changed. Two documents saving at
// the same time touch different keys, and an unchanged tab never rewrites one.
export function draftStore() {
  const initial: Drafts = {};
  for (const name of keys()) {
    const draft = read(name);
    if (draft) initial[name.slice(prefix.length)] = draft;
  }
  const store = valueStore(initial);
  let saved = initial;
  function persist() {
    const current = store.get();
    if (current === saved) return;
    for (const id of new Set([
      ...Object.keys(saved),
      ...Object.keys(current),
    ])) {
      if (saved[id] === current[id]) continue;
      const draft = restored(current[id]);
      if (draft) deviceStorage.setItem(key(id), JSON.stringify(draft));
      else deviceStorage.removeItem(key(id));
    }
    saved = current;
  }
  return {
    ...store,
    clear() {
      saved = {};
      store.set(saved);
      for (const name of keys()) deviceStorage.removeItem(name);
    },
    listen() {
      const save = scheduleSave(persist, draftSaveDelayMs);
      const queue = () => {
        if (saved !== store.get()) save.queue();
      };
      const unsubscribe = store.subscribe(queue);
      queue();
      // A saved change from another tab is visible here too. Local unsaved
      // edits to the same conversation keep their turn to write.
      const changed = (event: StorageEvent) => {
        if (!event.key?.startsWith(prefix)) return;
        const id = event.key.slice(prefix.length);
        const current = store.get();
        if (current[id] !== saved[id]) return;
        const draft = read(event.key);
        const next = replace(current, id, draft);
        saved = current === saved ? next : replace(saved, id, draft);
        store.set(next);
      };
      addEventListener("storage", changed);
      return () => {
        unsubscribe();
        removeEventListener("storage", changed);
        save.stop();
      };
    },
  };
}
