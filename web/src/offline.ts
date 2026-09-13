import type { Catalogue, Draft, Snapshot, View } from "./types.ts";
import { api, readPages } from "./store.ts";

const LIMIT = 50 * 1024 * 1024;
const POINTER = "uagent-offline-device";
export interface Download {
  id: string;
  title: string;
  bytes: number;
  touched: number;
  pinned: boolean;
  complete: boolean;
}
const request = <T>(value: IDBRequest<T>) =>
  new Promise<T>((resolve, reject) => {
    value.onsuccess = () => resolve(value.result);
    value.onerror = () => reject(value.error);
  });
const completed = (tx: IDBTransaction) =>
  new Promise<void>((resolve, reject) => {
    tx.oncomplete = () => resolve();
    tx.onabort = tx.onerror = () =>
      reject(tx.error || new Error("Offline storage failed"));
  });
export function cachedSnapshot(snapshot: Snapshot): Snapshot {
  const { state, metadata } = snapshot;
  const view = state?.view;
  return {
    cursor: snapshot.cursor,
    epoch: snapshot.epoch,
    metadata: {
      ...metadata,
      turn_active: false,
      pending: false,
      presence: "",
      status: "offline",
    },
    state: state && {
      ...state,
      http: undefined,
      activity: "Offline",
      activities: [],
      collaborators: [],
      view: view && {
        ...view,
        blocks: view.blocks.map(({ http: _http, ...block }) => block),
      },
    },
  };
}
export function mergeCached(
  previous: Snapshot | undefined,
  latest: Snapshot,
): Snapshot {
  const a = previous?.state?.view,
    b = latest.state?.view;
  if (
    !a ||
    !b ||
    previous?.epoch !== latest.epoch ||
    a.dropped_segments !== b.dropped_segments
  )
    return latest;
  const first = b.before || b.blocks[0]?.sequence;
  if (!first) return latest;
  const older = a.blocks.filter((block) => (block.sequence || 0) < first);
  const old = new Map(a.blocks.map((block) => [block.id, block]));
  const blocks = b.blocks.map((block) => {
    const prior = old.get(block.id);
    return prior &&
      !prior.truncated &&
      block.truncated &&
      prior.text?.startsWith(block.text || "")
      ? { ...block, text: prior.text, truncated: false }
      : block;
  });
  return {
    ...latest,
    state: {
      ...latest.state,
      view: {
        ...b,
        blocks: [...older, ...blocks],
        before: older.length ? a.before : b.before,
        more: older.length ? a.more : b.more,
      },
    },
  };
}
class OfflineStorage {
  private device = "";
  private db?: Promise<IDBDatabase>;
  private generation = 0;
  private writes = Promise.resolve();
  private downloadsInProgress = new AbortController();
  private invalidate() {
    this.generation++;
    this.downloadsInProgress.abort();
    this.downloadsInProgress = new AbortController();
  }
  enabled = localStorage.getItem("uagent-offline-enabled") !== "false";
  private changed() {
    dispatchEvent(new Event("uagent-offline-change"));
  }
  select(device: string) {
    if (!device || device === this.device) return;
    this.invalidate();
    this.db?.then((db) => db.close());
    this.device = device;
    localStorage.setItem(POINTER, device);
    this.db = undefined;
  }
  private open() {
    if (!this.device || !this.enabled)
      return Promise.reject(new Error("Offline storage is disabled"));
    localStorage.setItem(POINTER, this.device);
    return (this.db ??= new Promise<IDBDatabase>((resolve, reject) => {
      const op = indexedDB.open(`uagent-offline-${this.device}`, 1);
      op.onupgradeneeded = () => {
        op.result.createObjectStore("snapshots");
        op.result.createObjectStore("meta");
      };
      op.onsuccess = () => {
        op.result.onversionchange = () => op.result.close();
        resolve(op.result);
      };
      op.onerror = () => reject(op.error);
    }));
  }
  private async get<T>(store: string, key: string): Promise<T | undefined> {
    const db = await this.open();
    return request(db.transaction(store).objectStore(store).get(key));
  }
  async bootstrap() {
    const device = localStorage.getItem(POINTER);
    if (!device || !this.enabled) return undefined;
    this.select(device);
    const [catalogue, drafts] = await Promise.all([
      this.get<Catalogue>("meta", "catalogue"),
      this.get<Record<string, Draft>>("meta", "drafts"),
    ]);
    return catalogue && { catalogue, drafts: drafts || {} };
  }
  snapshot(id: string) {
    return this.get<Snapshot>("snapshots", id);
  }
  async downloads(): Promise<Download[]> {
    return this.enabled && this.device
      ? (await this.get<Download[]>("meta", "downloads")) || []
      : [];
  }
  private write(action: (db: IDBDatabase) => Promise<void>) {
    const generation = this.generation;
    const job = this.writes.then(async () => {
      if (generation !== this.generation || !this.enabled || !this.device)
        return;
      const db = await this.open();
      if (generation !== this.generation) return;
      await action(db);
      this.changed();
    });
    this.writes = job.catch(() => {});
    return job;
  }
  catalogue(list: Catalogue) {
    return this.write(async (db) => {
      const tx = db.transaction(["meta", "snapshots"], "readwrite"),
        done = completed(tx),
        meta = tx.objectStore("meta");
      const downloads: Download[] =
        (await request(meta.get("downloads"))) || [];
      const known = new Set(list.sessions.map((session) => session.id));
      for (const item of downloads)
        if (!known.has(item.id)) tx.objectStore("snapshots").delete(item.id);
      meta.put(
        downloads.filter((item) => known.has(item.id)),
        "downloads",
      );
      meta.put(
        {
          ...list,
          sessions: list.sessions.map((session) => ({
            ...session,
            presence: "",
            turn_active: false,
            pending: false,
            status: "offline",
          })),
          devices: [],
          capabilities: {},
        },
        "catalogue",
      );
      await done;
    });
  }
  drafts(drafts: Record<string, Draft>) {
    return this.write(async (db) => {
      const tx = db.transaction("meta", "readwrite"),
        done = completed(tx);
      tx.objectStore("meta").put(
        Object.fromEntries(
          Object.entries(drafts).map(([id, draft]) => [
            id,
            {
              text: draft.text,
              files: draft.files.filter((file) => !file.pending),
            },
          ]),
        ),
        "drafts",
      );
      await done;
    });
  }
  save(snapshot: Snapshot, pin?: boolean) {
    return this.write(async (db) => {
      const tx = db.transaction(["meta", "snapshots"], "readwrite"),
        done = completed(tx),
        meta = tx.objectStore("meta"),
        store = tx.objectStore("snapshots");
      const downloads: Download[] =
        (await request(meta.get("downloads"))) || [];
      const id = snapshot.metadata.id,
        previous: Snapshot | undefined = await request(store.get(id));
      const value = mergeCached(previous, cachedSnapshot(snapshot));
      const bytes = new Blob([JSON.stringify(value)]).size;
      const item = {
        id,
        title: snapshot.metadata.title,
        bytes,
        touched: Date.now(),
        pinned:
          pin ?? downloads.find((item) => item.id === id)?.pinned ?? false,
        complete:
          !snapshot.metadata.turn_active &&
          !!value.state?.view &&
          !value.state.view.more &&
          !value.state.view.blocks.some((block) => block.truncated),
      };
      const remaining = downloads
        .filter((item) => item.id !== id)
        .sort((a, b) => a.touched - b.touched);
      let total = bytes + remaining.reduce((sum, item) => sum + item.bytes, 0);
      for (let i = 0; total > LIMIT && i < remaining.length;) {
        if (remaining[i].pinned) {
          i++;
          continue;
        }
        const [evicted] = remaining.splice(i, 1);
        total -= evicted.bytes;
        store.delete(evicted.id);
      }
      if (total > LIMIT) {
        tx.abort();
        await done.catch(() => {});
        throw new Error(
          "Offline storage is full. Remove a pinned download to make space.",
        );
      }
      store.put(value, id);
      meta.put([...remaining, item], "downloads");
      await done;
    });
  }
  remove(id: string) {
    return this.write(async (db) => {
      const tx = db.transaction(["meta", "snapshots"], "readwrite"),
        done = completed(tx),
        meta = tx.objectStore("meta");
      const downloads: Download[] =
        (await request(meta.get("downloads"))) || [];
      meta.put(
        downloads.filter((item) => item.id !== id),
        "downloads",
      );
      tx.objectStore("snapshots").delete(id);
      await done;
    });
  }
  async clear() {
    this.invalidate();
    const device = this.device,
      pending = this.db;
    this.db = undefined;
    const db = pending && (await pending);
    db?.close();
    if (device)
      await new Promise<void>((resolve, reject) => {
        const op = indexedDB.deleteDatabase(`uagent-offline-${device}`);
        op.onsuccess = () => resolve();
        op.onerror = () => reject(op.error);
        op.onblocked = () =>
          reject(
            new Error("Close other µAgent tabs to clear offline storage."),
          );
      });
    if (this.device === device) localStorage.removeItem(POINTER);
    this.changed();
  }
  async toggle(enabled: boolean) {
    this.enabled = enabled;
    if (!enabled) await this.clear();
    localStorage.setItem("uagent-offline-enabled", String(enabled));
    if (enabled && this.device) localStorage.setItem(POINTER, this.device);
    this.changed();
  }
  async keep(id: string) {
    const signal = this.downloadsInProgress.signal;
    const snapshot = await api<Snapshot>(`/api/sessions/${id}`, undefined, {
      signal,
    });
    const initial = snapshot.state?.view;
    if (!initial) throw new Error("Conversation history is not available yet.");
    let view: View = initial;
    while (view.more) {
      const response: Snapshot = await api<Snapshot>(
        `/api/sessions/${id}?before=${view.before}`,
        undefined,
        { signal },
      );
      const page: View | undefined = response.state?.view;
      if (
        !page ||
        response.epoch !== snapshot.epoch ||
        response.metadata.generation !== snapshot.metadata.generation
      )
        throw new Error("History changed during download. Try again.");
      if (!page.before || page.before >= (view.before || Infinity))
        throw new Error("History changed during download. Try again.");
      view = { ...page, blocks: [...page.blocks, ...view.blocks] };
      if (new Blob([JSON.stringify(view)]).size > LIMIT)
        throw new Error("Conversation exceeds the offline storage budget.");
    }
    let remaining = LIMIT - new Blob([JSON.stringify(view)]).size;
    for (const block of view.blocks)
      if (block.truncated) {
        const tool = block.kind === "tool_result";
        const body = (
          await readPages(
            `/api/sessions/${id}?detail=${encodeURIComponent(tool ? block.detail_id || `t-${block.call_id}` : block.id)}${tool ? "&raw=1" : ""}`,
            signal,
            remaining,
          )
        ).text;
        remaining -= new Blob([body]).size;
        const output = tool ? JSON.parse(body).response : body;
        block.text =
          typeof output === "string" ? output : JSON.stringify(output, null, 2);
        block.truncated = false;
      }
    // Tool responses and original attachments can remain larger than their
    // display projection; complete refers to retained transcript pages.
    signal.throwIfAborted();
    await this.save({ ...snapshot, state: { ...snapshot.state, view } }, true);
  }
}
export const offline = new OfflineStorage();
