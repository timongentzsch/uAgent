import type {
  Block,
  BlockPatch,
  HostEvent,
  Snapshot,
  View,
} from "../shared/types.ts";
import { retainedBackgroundViews, maxHttpExchanges } from "../shared/limits.ts";
export function readStored<T>(
  storage: Pick<Storage, "getItem">,
  key: string,
  fallback: T,
): T {
  try {
    const value = JSON.parse(storage.getItem(key) || "null");
    return value !== null &&
      typeof value === typeof fallback &&
      Array.isArray(value) === Array.isArray(fallback)
      ? value
      : fallback;
  } catch {
    return fallback;
  }
}
export function writeStored(
  storage: Pick<Storage, "setItem">,
  key: string,
  value: unknown,
) {
  try {
    storage.setItem(key, JSON.stringify(value));
  } catch {
    /* Private storage may be full. */
  }
}

// The host's view is the one source of rows; the browser applies its patches
// in order. A new row lands in sequence position, so a late retained row
// never strands older content after newer rows.
export function applyBlock(view: View | undefined, patch: BlockPatch): View {
  const blocks = [...(view?.blocks || [])];
  const id = patch.block?.id ?? patch.id;
  const at = blocks.findIndex((block) => block.id === id);
  if (patch.block) {
    if (at >= 0) blocks[at] = patch.block;
    else {
      const sequence = patch.block.sequence;
      const before =
        typeof sequence === "number"
          ? blocks.findIndex(
              (block) =>
                typeof block.sequence === "number" && block.sequence > sequence,
            )
          : -1;
      blocks.splice(before < 0 ? blocks.length : before, 0, patch.block);
    }
  } else if (at >= 0) {
    const next: Block = { ...blocks[at], ...patch.set };
    if (patch.append?.text) next.text = (next.text || "") + patch.append.text;
    if (patch.append?.reasoning)
      next.reasoning = (next.reasoning || "") + patch.append.reasoning;
    blocks[at] = next;
  }
  return { ...view, blocks } as View;
}

// A checkpoint replaces the view; pages of older history the reader already
// loaded stay in front of it.
export function keepOlderPages(
  previous: Snapshot | undefined,
  latest: Snapshot,
): Snapshot {
  const a = previous?.state?.view,
    b = latest.state?.view;
  if (
    !a ||
    !b ||
    previous?.epoch !== latest.epoch ||
    // An in-place rewind starts a new view: older pages are gone with it.
    previous?.state?.view_epoch !== latest.state?.view_epoch ||
    a.dropped_segments !== b.dropped_segments
  )
    return latest;
  const first = b.before || b.blocks[0]?.sequence;
  if (!first) return latest;
  const seen = new Set(b.blocks.map((block) => block.id));
  const older = a.blocks.filter(
    (block) => (block.sequence || 0) < first && !seen.has(block.id),
  );
  if (!older.length) return latest;
  return {
    ...latest,
    state: {
      ...latest.state,
      view: {
        ...b,
        blocks: [...older, ...b.blocks],
        before: a.before,
        more: a.more,
      },
    },
  };
}

export function applySessionEvent(
  current: Snapshot,
  event: HostEvent,
): Snapshot {
  let state = current.state;
  const data = event.data || {};
  if (event.type === "usage.updated") {
    state = { ...(state || {}) };
    if (data.usage) state.usage = data.usage;
    if (data.statistics) state.statistics = data.statistics;
    if (data.context_tokens !== undefined)
      state.context_tokens = data.context_tokens;
  }
  if (event.kind === "activity")
    state = {
      ...(state || {}),
      activity: event.activity,
      phase: event.phase,
      activity_detail: event.activity_detail,
    };
  if (event.type === "activities.changed")
    state = { ...(state || {}), activities: data.activities };
  // Each exchange updates in place by id; earlier attempts stay reachable
  // through /http INDEX until the next published state replaces the list.
  if (event.type === "http.exchange" && data.id && data.state) {
    const exchange = {
      ...data,
      id: data.id,
      state: data.state,
      status: typeof data.status === "number" ? data.status : undefined,
    };
    const prior = state?.http || [];
    const at = prior.findIndex((item) => item.id === data.id);
    state = {
      ...(state || {}),
      http:
        at < 0
          ? [...prior, exchange].slice(-maxHttpExchanges)
          : prior.map((item, index) => (index === at ? exchange : item)),
    };
  }
  if (event.type === "config.changed" && data.permissions)
    state = { ...(state || {}), permissions: data.permissions };
  if (event.kind === "block")
    state = { ...(state || {}), view: applyBlock(state?.view, event) };
  const incoming = Math.max(
    current.metadata?.incoming || 0,
    (event.kind === "block" && event.block?.incoming) || 0,
  );
  const metadata =
    incoming === (current.metadata?.incoming || 0)
      ? current.metadata
      : { ...current.metadata, incoming };
  return {
    ...current,
    metadata,
    state,
    cursor: event.sequence,
  };
}

export function isIncoming(event: HostEvent) {
  return (
    event.type === "activity.completed" ||
    (event.kind === "block" &&
      (!!event.append?.text ||
        ["assistant", "activity"].includes(event.block?.kind || "")))
  );
}

export function isAttention(event: HostEvent) {
  return (
    event.type === "turn.completed" ||
    (event.type === "approval.requested" &&
      event.data?.route !== "coordinator") ||
    event.type === "approval.escalated" ||
    event.type === "error"
  );
}

// Keep the selected history and at most four active worker views. Browsing saved
// sessions must not accumulate full transcripts in the browser indefinitely.
export function retainedViews(
  views: Record<string, Snapshot>,
  selected: string,
): Record<string, Snapshot> {
  const recent = Object.entries(views)
    .filter(([id]) => id !== selected)
    .slice(-retainedBackgroundViews);
  return Object.fromEntries([
    ...recent,
    ...(views[selected] ? [[selected, views[selected]]] : []),
  ]);
}
