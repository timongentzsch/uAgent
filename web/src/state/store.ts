import type {
  Catalogue,
  Draft,
  ExecutionPhase,
  HostEvent,
  Outgoing,
  Session,
  Snapshot,
  State,
} from "../shared/types.ts";
import { retainedBackgroundViews, maxHttpExchanges } from "../shared/limits.ts";
import { mergeNodes, placeNode } from "../shared/tree.ts";

// Pure updates to snapshots, drafts and the catalogue for each kind of
// event. They keep object identity when nothing changed so views skip frames.

export const emptyDraft = (): Draft => ({ text: "", files: [] });
// Unsent work: text or a file, uploaded or still uploading.
export const hasContent = (draft: Draft) =>
  !!(draft.text || draft.files.length);

// A state frame replaces the session's state. Only checkpoint frames carry
// the view, HTTP log and prompt; the others keep the last ones.
export function stateFrame(
  prior: State | undefined,
  frame: State | undefined,
  phase: ExecutionPhase,
): State {
  return {
    view: prior?.view,
    http: prior?.http,
    system_prompt: prior?.system_prompt,
    ...frame,
    phase,
  };
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
    a.dropped_segments !== b.dropped_segments
  )
    return latest;
  const first = b.before || b.nodes[0]?.sequence;
  if (!first) return latest;
  const older = a.nodes.filter((node) => (node.sequence || 0) < first);
  const nodes = mergeNodes(older, b.nodes);
  if (nodes === b.nodes) return latest;
  return {
    ...latest,
    state: {
      ...latest.state,
      view: { ...b, nodes, before: a.before, more: a.more },
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
  if (event.kind === "turn_status")
    state = {
      ...(state || {}),
      turn_status: event.turn_status,
      phase: event.phase,
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
  if (event.kind === "node")
    state = {
      ...(state || {}),
      view: {
        ...state?.view,
        nodes: placeNode(state?.view?.nodes || [], event),
      },
    };
  const incoming = Math.max(
    current.metadata?.incoming || 0,
    (event.kind === "node" && event.node?.incoming) || 0,
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

// One session's catalogue entry changes in place; unchanged entries keep
// the catalogue's identity, so the shell skips the frame.
export function patchCatalogue(
  prior: Catalogue,
  id: string,
  change: (item: Session) => Session,
): Catalogue {
  let changed = false;
  const sessions = prior.sessions.map((item) => {
    if (item.id !== id) return item;
    const next = change(item);
    changed ||= next !== item;
    return next;
  });
  return changed ? { ...prior, sessions } : prior;
}

// Flat records with the same fields and values.
function sameFields(a: object, b: object) {
  const keys = Object.keys(a);
  return (
    keys.length === Object.keys(b).length &&
    keys.every(
      (key) =>
        (a as Record<string, unknown>)[key] ===
        (b as Record<string, unknown>)[key],
    )
  );
}

const sameActivities = (
  a: Session["activities"] = [],
  b: Session["activities"] = [],
) =>
  a.length === b.length && a.every((item, index) => sameFields(item, b[index]));

// A repeat of the activities it has keeps the session's identity.
export function withActivities(
  item: Session,
  activities: Session["activities"],
): Session {
  return sameActivities(item.activities, activities)
    ? item
    : { ...item, activities };
}

// A frame that restates a session unchanged keeps its identity, so what
// reads the session skips the frame.
export function keptSession(prior: Session | undefined, next: Session) {
  if (!prior) return next;
  const { activities: before, ...rest } = prior;
  const { activities: after, ...fields } = next;
  return sameFields(rest, fields) && sameActivities(before, after)
    ? prior
    : next;
}

// A session's count of arrivals only rises.
export function raiseIncoming(item: Session, incoming: number): Session {
  return (item.incoming || 0) < incoming ? { ...item, incoming } : item;
}

// A row the host sent replaces the outgoing one for its request. Unchanged
// state keeps its identity, so the shell skips the frame.
export function confirmOutgoing(
  items: Outgoing[],
  request_id: string | undefined,
): Outgoing[] {
  return items.some((item) => item.request_id === request_id)
    ? items.filter((item) => item.request_id !== request_id)
    : items;
}

export function isIncoming(event: HostEvent) {
  return (
    event.type === "activity.completed" ||
    (event.kind === "node" &&
      (!!event.append?.text ||
        event.node?.role === "assistant" ||
        event.node?.kind === "notice"))
  );
}

// Keep the selected history and at most four active worker views. Browsing saved
// sessions must not accumulate full transcripts in the browser indefinitely.
export function retainedViews(
  views: Record<string, Snapshot>,
  selected: string,
  watched: string[] = [],
): Record<string, Snapshot> {
  const pinned = new Set([selected, ...watched]);
  const recent = Object.entries(views)
    .filter(([id]) => !pinned.has(id))
    .slice(-retainedBackgroundViews);
  return Object.fromEntries([
    ...recent,
    ...Object.entries(views).filter(([id]) => pinned.has(id)),
  ]);
}
