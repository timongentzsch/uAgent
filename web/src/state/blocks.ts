import type { Block } from "../shared/types.ts";

// Live frames omit fields carried by retained blocks; absence never clears them.
export const defined = <T extends object>(patch: T): T =>
  Object.fromEntries(
    Object.entries(patch).filter(([, value]) => value !== undefined),
  ) as T;

export function toolIdentityKeys(row: Block, strongOnly = false): string[] {
  const keys: string[] = [];
  if (row.occurrence_id) keys.push(`o:${row.occurrence_id}`);
  if (row.call_id && row.response_id)
    keys.push(`r:${row.response_id}:${row.call_id}`);
  if (row.detail_id) keys.push(`d:${row.detail_id}`);
  if (!strongOnly && row.call_id) keys.push(`c:${row.call_id}`);
  return keys;
}

// Only model-invoked calls produce tool rows; async receipts arrive as message.changed.
// One incremental projection, also used to hydrate the server's replay.
// A retained block rejoins the live row it completes. Strong keys first;
// tool rows additionally match on call + detail identity: retained tool
// blocks can arrive before their occurrence facts are recorded (or via
// legacy paths), and an unmatched completion would strand its transient
// after newer rows. Detail ids are response-scoped when facts resolve,
// so a differing detail id on both sides vetoes the match — repeated
// provider call ids across turns must never cross-merge.
export function rejoinsToolRow(changed?: Block, item?: Block): boolean {
  if (!changed || !item) return false;
  // Single identity, occurrence first: same-response siblings must never
  // rejoin each other, only the row carrying the occurrence (or, without
  // one, the whole response).
  const identity = changed.occurrence_id || changed.response_id;
  if (
    identity &&
    (item.occurrence_id === identity || item.response_id === identity)
  )
    return true;
  return (
    changed.kind === "tool_result" &&
    item.kind === "tool_result" &&
    !!changed.call_id &&
    item.call_id === changed.call_id &&
    (changed.detail_id == null ||
      item.detail_id == null ||
      item.detail_id === changed.detail_id)
  );
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
  // A receipt-less retained block carries the fallback name "tool": keep
  // the live row's identity, take the retained body.
  const fallbackName =
    changed.receipt_missing && prior.name != null ? { name: prior.name } : {};
  return {
    ...prior,
    ...changed,
    ...fallbackName,
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
