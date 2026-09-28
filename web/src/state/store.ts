import { defined, reconcileBlock, rejoinsToolRow } from "./blocks.ts";
export { reconcileBlock } from "./blocks.ts";
import type { Block, HostEvent, Snapshot } from "../shared/types.ts";
import {
  maxLivePreviewChars,
  retainedBackgroundViews,
  maxHttpExchanges,
} from "../shared/limits.ts";
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

export function liveBlocks(events: HostEvent[], prior: Block[] = []): Block[] {
  let blocks = prior;
  const update = (index: number, change: Partial<Block>) => {
    const next = blocks.slice();
    next[index] = { ...next[index], ...defined(change) };
    blocks = next;
    return next[index];
  };
  for (const event of events) {
    const data = event.data || {};
    const responseId = data.response_id;
    let responseIndex = responseId
      ? blocks.findIndex((block) => block.response_id === responseId)
      : -1;
    if (event.type === "response.started" && responseId && responseIndex < 0) {
      blocks = [
        ...blocks,
        {
          id: responseId,
          response_id: responseId,
          request_id: data.request,
          kind: "assistant",
          text: "",
          reasoning: "",
          streaming: true,
          time: event.time,
        },
      ];
      responseIndex = blocks.length - 1;
    }
    if (
      event.type === "response.answer.delta" ||
      event.type === "response.reasoning.delta"
    ) {
      const field = event.type.includes("reasoning") ? "reasoning" : "text";
      if (responseIndex < 0) continue;
      const response = blocks[responseIndex];
      const text =
        (data.reset ? "" : response[field] || "") + (data.text || "");
      update(responseIndex, {
        [field]: text.slice(0, maxLivePreviewChars),
        truncated:
          response.truncated ||
          text.length > maxLivePreviewChars ||
          data.preview_truncated,
      });
    }
    if (event.type === "response.finished" && responseIndex >= 0)
      update(responseIndex, { streaming: false });
    if (event.type === "tool.call" || event.type === "tool.result") {
      if (!data.occurrence_id) continue;
      let index = blocks.findIndex(
        (item) => item.occurrence_id === data.occurrence_id,
      );
      if (index < 0) {
        blocks = [
          ...blocks,
          {
            id: data.occurrence_id,
            occurrence_id: data.occurrence_id,
            response_id: data.response_id,
            kind: "tool_result",
            call_id: data.call_id,
            detail_id: data.detail_id,
            time: event.time,
            name: data.name,
            text: "",
            status: "running",
          },
        ];
        index = blocks.length - 1;
      }
      update(
        index,
        event.type === "tool.call"
          ? {
              activity: data.activity,
              arguments: data.arguments,
              view: data.view,
            }
          : {
              activity: data.activity,
              parts: data.parts,
              text:
                typeof data.text === "string"
                  ? data.text
                  : typeof data.result === "string"
                    ? data.result
                    : JSON.stringify(data.result) || "",
              status:
                data.completion_status || String(data.status || "running"),
              duration_ms: data.duration_ms,
              change: data.presentation?.change,
            },
      );
    }
    if (event.type === "message.changed") {
      const block = data.block;
      blocks = blocks.filter((item) => !rejoinsToolRow(block, item));
    }
    if (event.type === "error")
      blocks = [
        ...blocks,
        {
          id: `error-${event.sequence || blocks.length}`,
          kind: "error",
          text: data.error || data.text || "Request failed",
        },
      ];
  }
  return blocks.slice(-64);
}

// Merge a fresh snapshot over the live view, keeping older retained pages
// and never downgrading a fuller block revision. Pure; used by the live
// event stream, independent of any persistence.
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
  const blocks = b.blocks.map((block) =>
    reconcileBlock(old.get(block.id), block),
  );
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

export function applySessionEvent(
  current: Snapshot,
  event: HostEvent,
): Snapshot {
  let state = current.state;
  const data = event.data || {};
  let streamed = current.streamed || [];
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
  if (event.type === "message.changed" && data.block) {
    const prior = state?.view;
    const transient = (current.streamed || []).find((block) =>
      rejoinsToolRow(data.block, block),
    );
    const changed = reconcileBlock(transient, data.block);
    const blocks = [...(prior?.blocks || [])];
    const index = blocks.findIndex(
      (block) =>
        block.id === changed.id ||
        (!!changed.occurrence_id &&
          block.occurrence_id === changed.occurrence_id) ||
        (changed.kind === "assistant" &&
          block.kind === "assistant" &&
          !!changed.response_id &&
          block.response_id === changed.response_id),
    );
    if (index < 0) {
      // Retained rows carry their sequence: insert in position so a late
      // arrival (replay, pre-facts emit) can never strand older content
      // after newer rows. Sequence-less rows keep their relative order.
      const sequence = (changed as Block).sequence;
      const at =
        typeof sequence === "number"
          ? blocks.findIndex(
              (block) =>
                typeof block.sequence === "number" && block.sequence > sequence,
            )
          : -1;
      if (at < 0) blocks.push(changed);
      else blocks.splice(at, 0, changed);
    } else blocks[index] = reconcileBlock(blocks[index], changed);
    state = {
      ...(state || {}),
      // Explicitly loaded older pages belong to this view until it is evicted.
      view: { ...prior, blocks },
    };
  }
  if (event.kind === "event") streamed = liveBlocks([event], streamed);
  const exchange = state?.http?.at(-1);
  if (exchange)
    streamed = streamed.map((block) =>
      block.streaming
        ? {
            ...block,
            http: state?.http,
            turn_root: exchange.turn_root,
            reply_to: exchange.reply_to,
            reply_excerpt: exchange.reply_excerpt,
          }
        : block,
    );
  const incoming = Math.max(
    current.metadata?.incoming || 0,
    data.block?.incoming || 0,
  );
  const metadata =
    incoming === (current.metadata?.incoming || 0)
      ? current.metadata
      : { ...current.metadata, incoming };
  return {
    ...current,
    metadata,
    state,
    streamed,
    cursor: event.sequence,
  };
}

export function isIncoming(event: HostEvent) {
  return (
    event.type === "response.answer.delta" ||
    event.type === "activity.completed" ||
    (event.type === "message.changed" &&
      ["assistant", "activity"].includes(event.data?.block?.kind || ""))
  );
}

export function isAttention(event: HostEvent) {
  return (
    event.type === "turn.completed" ||
    event.type === "approval.requested" ||
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
