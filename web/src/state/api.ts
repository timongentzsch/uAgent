import type {
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

export const protocol = 2;

export const requestId = () =>
  [...crypto.getRandomValues(new Uint8Array(16))]
    .map((value) => value.toString(16).padStart(2, "0"))
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

export function receiveOutcome(outcome: Outcome) {
  if (!outcome.pending) receipts.get(outcome.request_id)?.(outcome);
}

export async function command<K extends CommandKind>(
  kind: K,
  session?: SessionRef | null,
  fields: CommandFields = {},
  options: { signal?: AbortSignal } = {},
): Promise<CommandReceipt<K>> {
  options.signal?.throwIfAborted();
  const request_id = fields.request_id || requestId();
  let timeout: ReturnType<typeof setTimeout> | undefined;
  let abort: (() => void) | undefined;
  const receipt = new Promise<Outcome>((resolve) => {
    abort = () => resolve({ request_id, accepted: true, pending: true });
    options.signal?.addEventListener("abort", abort, { once: true });
    receipts.set(request_id, resolve);
    timeout = setTimeout(
      () => resolve({ request_id, accepted: true, pending: true }),
      commandReceiptWaitMs,
    );
  });
  try {
    let result = await api<Outcome>(
      "/api/command",
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

export async function manage<K extends CommandKind>(
  kind: K,
  fields: CommandFields = {},
  signal?: AbortSignal,
): Promise<CommandResults[K]> {
  const result = await command(kind, null, fields, { signal });
  if (result.pending)
    throw new Error(
      "The operation is still pending. Refresh to inspect its result.",
    );
  return result.result;
}
