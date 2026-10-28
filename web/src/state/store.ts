import type {
  Block,
  BlockPatch,
  Catalogue,
  Draft,
  ExecutionPhase,
  HostEvent,
  Node,
  NodeView,
  Outgoing,
  Session,
  Snapshot,
  State,
  ToolView,
  View,
} from "../shared/types.ts";
import { retainedBackgroundViews, maxHttpExchanges } from "../shared/limits.ts";

// Pure updates to snapshots, drafts and the catalogue for each kind of
// event. They keep object identity when nothing changed so views skip frames.

export const emptyDraft = (): Draft => ({ text: "", files: [] });
// Unsent work: text or a file, uploaded or still uploading.
export const hasContent = (draft: Draft) =>
  !!(draft.text || draft.files.length);

// Temporary, until this page draws the host's nodes (stage 3 of the
// transcript refactor removes it with the fold logic in message-view.ts):
// the host sends its transcript as nodes, and the page adopts each as the
// block it drew before. A reply's thinking joins the reply, a group's verb
// joins its calls, and the host's work nodes are left to the page's folds.
const groupLabels = new Map<string, string>();
const kThinking = ":thinking";

function blockOf(node: Node): Block | undefined {
  const { parent, kind, body, headline, input, routine, category, ...rest } =
    node;
  const base = {
    ...rest,
    text: body?.kind === "diff" ? undefined : body?.text,
    truncated: body?.truncated,
  };
  const view: ToolView | undefined = headline && {
    ...(headline.verb && {
      verb: [headline.verb, headline.verb] as [string, string],
    }),
    target: headline.target,
    input,
  };
  const label = headline?.verb ? undefined : headline?.target;
  if (kind === "group" && headline)
    groupLabels.set(node.id, `${headline.verb} · ${node.result?.word}`);
  if (kind === "turn") return { ...base, kind: "user", turn_root: node.id };
  if (kind === "message" && node.role === "assistant")
    return { ...base, kind: "assistant", response_id: node.id };
  if (kind === "message" && node.origin === "tool")
    return { ...base, kind: "attachment", source_call_ids: [parent || ""] };
  if (kind === "message") return { ...base, kind: "user" };
  if (kind === "footer") return { ...base, kind: "turn_summary" } as Block;
  if (kind === "notice" && node.compaction)
    return { ...base, kind: "compaction" } as Block;
  if (kind === "notice")
    return {
      ...base,
      kind: "activity",
      view,
      body_kind: body?.kind,
      activity: { category, label },
      memory: node.memory && { ...node.memory, minor: routine },
    };
  if (kind === "call")
    return {
      ...base,
      kind: "tool_result",
      call_id: node.id,
      detail_id: node.id,
      view,
      tail: body?.tail,
      change: body?.kind === "diff" ? body.text : undefined,
      body_kind: body?.kind === "diff" ? undefined : body?.kind,
      activity: {
        category,
        label,
        group: parent?.startsWith("g-")
          ? { id: parent, label: groupLabels.get(parent) || "" }
          : undefined,
      },
    };
  return undefined;
}

// A view the host sent, as the blocks this page draws.
export function adoptView(view: NodeView): View;
export function adoptView(view: NodeView | undefined): View | undefined;
export function adoptView(view: NodeView | undefined) {
  if (!view) return view;
  const blocks: Block[] = [];
  const thinking = new Map<string, string>();
  for (const node of view.blocks) {
    if (node.kind === "thinking")
      thinking.set(node.id.slice(0, -kThinking.length), node.body?.text || "");
    const block = blockOf(node);
    if (block?.kind === "assistant") block.reasoning = thinking.get(block.id);
    if (block) blocks.push(block);
  }
  return { ...view, blocks };
}

// State the host sent, with its view adopted.
export function adoptState<S extends { view?: View }>(state: S): S;
export function adoptState<S extends { view?: View }>(
  state: S | undefined,
): S | undefined;
export function adoptState<S extends { view?: View }>(state: S | undefined) {
  return state?.view
    ? { ...state, view: adoptView(state.view as unknown as NodeView) }
    : state;
}

// A snapshot the host sent, with its view adopted.
export const adoptSnapshot = (snapshot: Snapshot): Snapshot => ({
  ...snapshot,
  state: adoptState(snapshot.state),
});

// The host's view is the one source of rows; the browser applies its patches
// in order. A new row lands in sequence position, so a late retained row
// never strands older content after newer rows.
function applyBlock(view: View | undefined, patch: BlockPatch): View {
  const blocks = [...(view?.blocks || [])];
  const id = patch.block?.id ?? patch.id ?? "";
  // A reply's thinking is drawn on the reply.
  const reply = id.endsWith(kThinking) ? id.slice(0, -kThinking.length) : "";
  // Streaming patches the newest rows: search from the end.
  const at = blocks.findLastIndex((block) => block.id === (reply || id));
  const block = patch.block && blockOf(patch.block);
  // A group's calls arrive before it, and take its words when it does.
  if (patch.block?.kind === "group") {
    const group = { id, label: groupLabels.get(id) || "" };
    return {
      ...view,
      blocks: blocks.map((held) =>
        held.kind === "tool_result" && held.activity?.group?.id === id
          ? { ...held, activity: { ...held.activity, group } }
          : held,
      ),
    } as View;
  }
  if (reply) {
    // A reply thinking before its first text is held as an empty one.
    const held: Block | undefined =
      at >= 0
        ? blocks[at]
        : patch.block && { id: reply, kind: "assistant", streaming: true };
    if (held?.kind !== "assistant") return view || { blocks };
    const text = patch.block?.body?.text;
    const next = {
      ...held,
      reasoning: text ?? (held.reasoning || "") + (patch.append?.text || ""),
    };
    if (at >= 0) blocks[at] = next;
    else blocks.push(next);
  } else if (block) {
    if (at >= 0)
      blocks[at] =
        block.kind === "assistant"
          ? { ...block, reasoning: (blocks[at] as typeof block).reasoning }
          : block;
    else {
      const sequence = block.sequence;
      const before =
        typeof sequence === "number"
          ? blocks.findIndex(
              (held) =>
                typeof held.sequence === "number" && held.sequence > sequence,
            )
          : -1;
      blocks.splice(before < 0 ? blocks.length : before, 0, block);
    }
  } else if (at >= 0 && !patch.block) {
    const next: Block = { ...blocks[at], ...patch.set };
    if (patch.append?.text) next.text = (next.text || "") + patch.append.text;
    blocks[at] = next;
  }
  return { ...view, blocks } as View;
}

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
  if (event.kind === "block")
    state = { ...(state || {}), view: applyBlock(state?.view, event) };
  const incoming = Math.max(
    current.metadata?.incoming || 0,
    (event.kind === "block" &&
      event.block &&
      "incoming" in event.block &&
      event.block.incoming) ||
      0,
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
    (event.kind === "block" &&
      (!!event.append?.text ||
        event.block?.role === "assistant" ||
        event.block?.kind === "notice"))
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
