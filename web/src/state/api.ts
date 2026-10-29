import type {
  Asset,
  BodyPage,
  CommandFields,
  CommandResults,
  CommandKind,
  CommandReceipt,
  Outcome,
  SessionRef,
} from "../shared/types.ts";
import { commandReceiptWaitMs } from "../shared/limits.ts";
import { failure } from "../shared/types.ts";

// Every call the page makes to the host: the route table, `api` for plain
// requests, `command` for the host's command protocol with its receipts, and
// uploads and paged reads.

export const protocol = 2;

// Every host path the page calls, so a path is written once.
export const routes = {
  auth: "/api/auth",
  browserStatus: "/api/browser/status",
  browserViewer: "/api/browser/viewer",
  command: "/api/command",
  terminals: "/api/terminals",
  // A terminal's socket.
  terminal: (id: string) => `/api/terminals/${id}`,
  pushSubscriptions: "/api/push/subscriptions",
  // A folder at a glance: its spend, changes and git.
  folder: (path: string) => `/api/folder?path=${encodeURIComponent(path)}`,
  sessions: "/api/sessions",
  refreshedSessions: "/api/sessions?refresh=1",
  events: (cursor: string) => `/api/events?cursor=${cursor}`,
  receipt: (requestId: string) => `/api/receipts/${requestId}`,
  session: (id: string, query = "") =>
    `/api/sessions/${id}${query && `?${query}`}`,
  assets: (session: string) => `/api/sessions/${session}/assets/`,
  asset: (session: string, id: string) =>
    `/api/sessions/${session}/assets/${id}`,
  attachments: (session: string, name: string) =>
    `/api/sessions/${session}/attachments?name=${encodeURIComponent(name)}`,
  // Rows that contain q, in every conversation or, with a session, in one.
  search: (q: string, session?: string) =>
    `/api/search?q=${encodeURIComponent(q)}${session ? `&session=${session}` : ""}`,
  // The conversation's folder, read only: `list`, `read` and `find` answer
  // JSON, `raw` is the file itself.
  files: (
    session: string,
    mode: "list" | "read" | "raw" | "find",
    value: string,
  ) => `/api/sessions/${session}/files?${mode}=${encodeURIComponent(value)}`,
};

// A random 32-hex id for a command: the host answers and deduplicates by it.
export const requestId = () =>
  [...crypto.getRandomValues(new Uint8Array(16))]
    .map((value) => value.toString(16).padStart(2, "0"))
    .join("");

// A request to the host: a GET, or a POST of `body` as JSON. Resolves to the
// parsed answer. A failure throws with `status` set when the host answered,
// `network` when nothing did, and `rejected` when it refused a command.
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
  // A proxy's HTML error page still carries its status to the caller.
  const data = await response.json().catch(() => null);
  if (!response.ok || !data || data.accepted === false) {
    const error = failure(new Error(data?.error || `HTTP ${response.status}`));
    error.status = response.status;
    error.rejected = data?.accepted === false;
    throw error;
  }
  if (data.v !== undefined && data.v !== protocol)
    throw new Error(
      "App/server version differs. Save your draft, then update this app.",
    );
  return data as T;
}

// Stores a file among the session's attachments. Its id then rides a
// command's attachment_ids, which the host claims and resolves.
export function uploadAttachment(session: string, file: File, name: string) {
  return api<Asset>(routes.attachments(session, name), undefined, {
    method: "POST",
    headers: { "Content-Type": file.type || "application/octet-stream" },
    body: file,
  });
}

// Reads a body the host serves in pages and joins it: `path` ends in a query
// and gets `&offset=`, or is a function of the offset. `limit` is in bytes of
// text; past it the read fails.
export async function readPages(
  path: string | ((offset: number) => Promise<BodyPage>),
  signal?: AbortSignal,
  limit = Infinity,
) {
  const parts: string[] = [];
  let size = 0;
  let offset = 0,
    page: BodyPage;
  do {
    signal?.throwIfAborted();
    page =
      typeof path === "function"
        ? await path(offset)
        : await api<BodyPage>(`${path}&offset=${offset}`, undefined, {
            signal,
          });
    signal?.throwIfAborted();
    size += new Blob([page.text || ""]).size;
    if (size > limit) throw new Error("Content exceeds the download budget.");
    parts.push(page.text || "");
    if (page.more && page.next <= offset) throw new Error("Incomplete body.");
    offset = page.next;
  } while (page.more);
  return { ...page, text: parts.join("") };
}

const receipts = new Map<string, (outcome: Outcome) => void>();

// True when a command from this page waits on it: its caller shows the
// outcome, so nothing else needs to.
export function receiveOutcome(outcome: Outcome) {
  const waiting = receipts.get(outcome.request_id);
  if (!outcome.pending) waiting?.(outcome);
  return !!waiting;
}

export async function command<K extends CommandKind>(
  kind: K,
  session?: SessionRef | null,
  fields?: CommandFields<K>,
  options: { signal?: AbortSignal } = {},
): Promise<CommandReceipt<K>> {
  options.signal?.throwIfAborted();
  const request_id = fields?.request_id || requestId();
  let timeout: ReturnType<typeof setTimeout> | undefined;
  let abort: (() => void) | undefined;
  const receipt = new Promise<Outcome>((resolve) => {
    abort = () => resolve({ request_id, accepted: true, pending: true });
    options.signal?.addEventListener("abort", abort, { once: true });
    receipts.set(request_id, resolve);
    timeout = setTimeout(abort, commandReceiptWaitMs);
  });
  try {
    let result = await api<Outcome>(
      routes.command,
      {
        v: protocol,
        request_id,
        kind,
        ...(session
          ? { session_id: session.id, generation: session.generation }
          : {}),
        ...fields,
      },
      options,
    );
    if (result.pending) result = await receipt;
    options.signal?.throwIfAborted();
    if (result.accepted === false) {
      const error = failure(new Error(result.error || "Command rejected"));
      error.rejected = true;
      throw error;
    }
    return result as CommandReceipt<K>;
  } finally {
    clearTimeout(timeout);
    receipts.delete(request_id);
    if (abort) options.signal?.removeEventListener("abort", abort);
  }
}

// A command whose answer is its result: the host's own, or a conversation's
// when `session` names one. A result that has not arrived in time is an
// error here, so callers handle one outcome.
export async function manage<K extends CommandKind>(
  kind: K,
  fields?: CommandFields<K>,
  {
    session = null,
    signal,
  }: { session?: SessionRef | null; signal?: AbortSignal } = {},
): Promise<CommandResults[K]> {
  const result = await command(kind, session, fields, { signal });
  if (result.pending)
    throw new Error(
      "The operation is still pending. Refresh to inspect its result.",
    );
  return result.result;
}
