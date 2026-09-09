import type {
  Block,
  HostEvent,
  Snapshot,
  Outcome,
  CommandReceipt,
  CommandKind,
  CommandFields,
  SessionRef,
  BodyPage,
} from "./types.ts";
import { failure } from "./types.ts";
export const protocol = 1;
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
export const requestId = () =>
  [...crypto.getRandomValues(new Uint8Array(16))]
    .map((x) => x.toString(16).padStart(2, "0"))
    .join("");
export async function api<T = unknown>(
  path: string,
  body?: unknown,
  options: RequestInit = {},
): Promise<T> {
  const response = await fetch(path, {
    credentials: "same-origin",
    cache: "no-store",
    ...(body === undefined
      ? {}
      : {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(body),
        }),
    ...options,
  }).catch((error) => {
    const result = failure(error);
    result.network = true;
    throw result;
  });
  const data = await response.json();
  if (!response.ok || data.accepted === false) {
    const error = failure(new Error(data.error || `HTTP ${response.status}`));
    error.status = response.status;
    error.rejected = data.accepted === false;
    throw error;
  }
  if (data.v !== undefined && data.v !== protocol)
    throw new Error(
      "App/server version differs. Save your draft, then update this app.",
    );
  return data as T;
}
export async function readPages(path: string, signal?: AbortSignal) {
  const parts: string[] = [];
  let offset = 0,
    page: BodyPage;
  do {
    page = await api<BodyPage>(`${path}&offset=${offset}`, undefined, {
      signal,
    });
    parts.push(page.text || "");
    if (page.more && page.next <= offset) throw new Error("Incomplete body.");
    offset = page.next;
  } while (page.more);
  return { ...page, text: parts.join("") };
}
const receipts = new Map<string, (outcome: Outcome) => void>();
export function receiveOutcome(outcome: Outcome) {
  receipts.get(outcome.request_id)?.(outcome);
}
export async function command<K extends CommandKind>(
  kind: K,
  session?: SessionRef | null,
  fields: CommandFields = {},
): Promise<CommandReceipt<K>> {
  const request_id = fields.request_id || requestId();
  let timeout: ReturnType<typeof setTimeout> | undefined;
  const receipt = new Promise<Outcome>((resolve) => {
    receipts.set(request_id, resolve);
    timeout = setTimeout(
      () => resolve({ request_id, accepted: true, pending: true }),
      30000,
    );
  });
  try {
    let result = await api<Outcome>("/api/command", {
      v: protocol,
      request_id,
      kind,
      ...(session
        ? { session_id: session.id, generation: session.generation }
        : {}),
      ...fields,
    });
    if (result.pending) result = await receipt;
    if (result.accepted === false) {
      const error = failure(new Error(result.error || "Command rejected"));
      error.rejected = true;
      throw error;
    }
    return result as CommandReceipt<K>;
  } finally {
    clearTimeout(timeout);
    receipts.delete(request_id);
  }
}

// One incremental projection, also used to hydrate the server's replay.
export function liveBlocks(events: HostEvent[], prior: Block[] = []): Block[] {
  const blocks = prior.map((block) => ({ ...block }));
  for (const event of events) {
    const data = event.data || {};
    let response = blocks.findLast((block) => block.kind === "assistant");
    if (
      event.type === "response.started" ||
      ((!response || !response.streaming) && event.type?.endsWith(".delta"))
    ) {
      response = {
        id: `live-${event.sequence || event.time || blocks.length}`,
        kind: "assistant",
        text: "",
        reasoning: "",
        streaming: true,
        time: event.time,
      };
      blocks.push(response);
    }
    if (
      event.type === "response.answer.delta" ||
      event.type === "response.reasoning.delta"
    ) {
      const field = event.type.includes("reasoning") ? "reasoning" : "text";
      if (!response) continue;
      const text = (response[field] || "") + (data.text || "");
      response[field] = text.slice(0, 64 * 1024);
      response.truncated ||= text.length > 64 * 1024 || data.preview_truncated;
    }
    if (event.type === "response.finished" && response)
      response.streaming = false;
    if (event.type === "tool.call" || event.type === "tool.result") {
      const id = `tool-${data.id}`;
      let block = blocks.find((item) => item.id === id);
      if (!block) {
        block = {
          id,
          kind: "tool_result",
          call_id: data.id,
          time: event.time,
          name: data.name,
          text: "",
          status: "running",
        };
        blocks.push(block);
      }
      if (event.type === "tool.call") block.arguments = data.arguments;
      else
        Object.assign(block, {
          text:
            typeof data.result === "string"
              ? data.result
              : JSON.stringify(data.result) || "",
          status:
            data.completion_status || String(data.status || "not recorded"),
          duration_ms: data.duration_ms,
          change: data.presentation?.change,
        });
    }
    if (event.type === "message.changed") {
      const block = data.block;
      if (block?.kind === "assistant") {
        const index = blocks.findLastIndex((item) => item.kind === "assistant");
        if (index >= 0) blocks.splice(index, 1);
      } else if (block?.kind === "tool_result") {
        const index = blocks.findIndex(
          (item) => item.call_id === block.call_id,
        );
        if (index >= 0) blocks.splice(index, 1);
      }
    }
    if (event.type === "error")
      blocks.push({
        id: `error-${event.sequence || blocks.length}`,
        kind: "error",
        text: data.error || data.text || "Request failed",
      });
  }
  return blocks.slice(-64);
}

export function applySessionEvent(
  current: Snapshot,
  event: HostEvent,
): Snapshot {
  const state = { ...(current.state || {}) };
  const data = event.data || {};
  let streamed = current.streamed || liveBlocks(current.live || []);
  if (event.kind === "activity") state.activity = event.activity;
  if (event.type === "activities.changed") state.activities = data.activities;
  if (event.type === "http.exchange" && data.id && data.state)
    state.http = [
      {
        ...data,
        id: data.id,
        state: data.state,
        status: typeof data.status === "number" ? data.status : undefined,
      },
    ];
  if (event.type === "config.changed" && data.permissions)
    state.permissions = data.permissions;
  if (event.type === "message.changed" && data.block) {
    const prior = state.view;
    const changed = data.block;
    const blocks = [...(prior?.blocks || [])];
    const index = blocks.findIndex((block) => block.id === changed.id);
    if (index < 0) blocks.push(data.block);
    else blocks[index] = data.block;
    state.view = { ...prior, blocks: blocks.slice(-256) };
  }
  if (event.kind === "event") streamed = liveBlocks([event], streamed);
  const exchange = state.http?.at(-1);
  if (exchange)
    streamed = streamed.map((block) =>
      block.streaming
        ? {
            ...block,
            http: state.http,
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
  return {
    ...current,
    metadata: { ...current.metadata, incoming },
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
