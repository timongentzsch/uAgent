import type { Block, Collaborator, HostEvent, Snapshot } from "./types.ts";
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

// Only model-invoked calls produce tool rows; async receipts arrive as message.changed.
// One incremental projection, also used to hydrate the server's replay.
export function liveBlocks(events: HostEvent[], prior: Block[] = []): Block[] {
  let blocks = prior;
  const update = (index: number, change: Partial<Block>) => {
    const next = blocks.slice();
    next[index] = { ...next[index], ...change };
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
      const text = (response[field] || "") + (data.text || "");
      update(responseIndex, {
        [field]: text.slice(0, 64 * 1024),
        truncated:
          response.truncated ||
          text.length > 64 * 1024 ||
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
          ? { activity: data.activity, arguments: data.arguments }
          : {
              activity: data.activity,
              text:
                typeof data.result === "string"
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
      const identity = block?.occurrence_id || block?.response_id;
      if (identity)
        blocks = blocks.filter(
          (item) =>
            item.response_id !== identity && item.occurrence_id !== identity,
        );
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

export function reconcileBlock(
  prior: Block | undefined,
  changed: Block,
): Block {
  if (!prior) return changed;
  const sameContent =
    prior.content_revision === undefined ||
    prior.content_revision === changed.content_revision;
  const sameReasoning =
    prior.reasoning_revision === undefined ||
    prior.reasoning_revision === changed.reasoning_revision;
  const priorTextBytes = prior.text_bytes ?? (prior.text || "").length;
  const changedTextBytes = changed.text_bytes ?? (changed.text || "").length;
  const priorReasoningBytes =
    prior.reasoning_bytes ?? (prior.reasoning || "").length;
  const changedReasoningBytes =
    changed.reasoning_bytes ?? (changed.reasoning || "").length;
  const preserveText = sameContent && priorTextBytes > changedTextBytes;
  const preserveReasoning =
    sameReasoning && priorReasoningBytes > changedReasoningBytes;
  return {
    ...prior,
    ...changed,
    text: preserveText ? prior.text : changed.text,
    reasoning: preserveReasoning ? prior.reasoning : changed.reasoning,
    truncated: preserveText ? prior.truncated || false : changed.truncated,
    content_complete: preserveText
      ? (prior.content_complete ?? !prior.truncated)
      : changed.content_complete,
    reasoning_complete: preserveReasoning
      ? (prior.reasoning_complete ?? true)
      : changed.reasoning_complete,
    text_bytes: preserveText ? priorTextBytes : changed.text_bytes,
    reasoning_bytes: preserveReasoning
      ? priorReasoningBytes
      : changed.reasoning_bytes,
  };
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
    state = { ...(state || {}), activity: event.activity };
  if (event.type === "activities.changed")
    state = { ...(state || {}), activities: data.activities };
  if (event.type === "collaborator.changed" && data.collaborator) {
    state = { ...(state || {}) };
    const collaborators = [...(state.collaborators || [])];
    const changed = data.collaborator as unknown as Collaborator;
    const index = collaborators.findIndex((item) => item.id === changed.id);
    if (data.removed) {
      if (index >= 0) collaborators.splice(index, 1);
    } else if (index < 0) {
      collaborators.push(changed);
    } else {
      collaborators[index] = changed;
    }
    state.collaborators = collaborators;
  }
  if (event.type === "http.exchange" && data.id && data.state)
    state = {
      ...(state || {}),
      http: [
        {
          ...data,
          id: data.id,
          state: data.state,
          status: typeof data.status === "number" ? data.status : undefined,
        },
      ],
    };
  if (event.type === "config.changed" && data.permissions)
    state = { ...(state || {}), permissions: data.permissions };
  if (event.type === "message.changed" && data.block) {
    const prior = state?.view;
    const transient = (current.streamed || []).find(
      (block) =>
        (!!data.block?.response_id &&
          block.response_id === data.block.response_id) ||
        (!!data.block?.occurrence_id &&
          block.occurrence_id === data.block.occurrence_id),
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
    if (index < 0) blocks.push(changed);
    else blocks[index] = reconcileBlock(blocks[index], changed);
    state = {
      ...(state || {}),
      view: { ...prior, blocks: blocks.slice(-256) },
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
    .slice(-4);
  return Object.fromEntries([
    ...recent,
    ...(views[selected] ? [[selected, views[selected]]] : []),
  ]);
}
