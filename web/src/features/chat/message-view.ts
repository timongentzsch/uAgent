import type { Block, PresentedBlock } from "../../shared/types.ts";
import { isRunningStatus } from "../../shared/display.ts";

// Flat, stable, uniform rows. Every row renders the same chrome, so there is
// no header/matrix state to derive (and nothing to drift). Two things
// happen here: tool calls and their results join into one row per call
// within a turn, and empty assistant placeholders (response.started
// before the first delta) are dropped: the status line already covers
// the live state.
// Both preserve input objects and keep stable keys across streaming deltas.
export function presentMessages(blocks: Block[]): PresentedBlock[] {
  const rows: PresentedBlock[] = [];
  // One row per tool call. A call can arrive as a live frame, a retained
  // assistant-tools entry and a retained result block — in any order, on
  // either side of a snapshot refresh, each carrying a different subset
  // of keys. Every key a row carries is registered, so late arrivals
  // merge instead of rendering as stuck "Running" ghosts plus orphan
  // result rows. Strong keys (occurrence, response:call, detail) pin one
  // call globally and keep their first row; the bare call id is only
  // positional within a turn, so it tracks the latest row — repeated
  // provider ids across turns stay isolated by the user-boundary reset
  // below, and retries rejoin their call.
  const calls = new Map<string, number>();
  const strongKeysOf = (
    row: Pick<
      PresentedBlock,
      "occurrence_id" | "response_id" | "call_id" | "detail_id"
    >,
  ): string[] => {
    const keys: string[] = [];
    if (row.occurrence_id) keys.push(`o:${row.occurrence_id}`);
    if (row.call_id && row.response_id)
      keys.push(`r:${row.response_id}:${row.call_id}`);
    if (row.detail_id) keys.push(`d:${row.detail_id}`);
    return keys;
  };
  const weakKeysOf = (row: Pick<PresentedBlock, "call_id">): string[] =>
    row.call_id ? [`c:${row.call_id}`] : [];
  // Splits only rejoin on strong identity: a call record must never glue
  // itself to an unrelated orphan that merely shares a bare call id.
  const findStrong = (
    row: Pick<
      PresentedBlock,
      "occurrence_id" | "response_id" | "call_id" | "detail_id"
    >,
  ): number | undefined => {
    for (const key of strongKeysOf(row)) {
      const index = calls.get(key);
      if (index !== undefined && rows[index]) return index;
    }
    return undefined;
  };
  // Plain result rows seek their call on every key, weakest last.
  const find = (
    row: Pick<
      PresentedBlock,
      "occurrence_id" | "response_id" | "call_id" | "detail_id"
    >,
  ): number | undefined => {
    for (const key of [...strongKeysOf(row), ...weakKeysOf(row)]) {
      const index = calls.get(key);
      if (index !== undefined && rows[index]) return index;
    }
    return undefined;
  };
  const register = (
    row: Pick<
      PresentedBlock,
      "occurrence_id" | "response_id" | "call_id" | "detail_id"
    >,
    index: number,
  ) => {
    for (const key of strongKeysOf(row)) {
      if (!calls.has(key)) calls.set(key, index);
    }
    // Positional fallback: later rows in the same turn supersede.
    for (const key of weakKeysOf(row)) calls.set(key, index);
  };
  // A completed row never regresses: a stale call-state arrival (still
  // "Running", no duration) must not wipe the result that already
  // landed from another path. Absent fields never wipe present ones:
  // live frames omit keys the retained path carries and vice versa.
  const defined = (patch: Partial<PresentedBlock>) =>
    Object.fromEntries(
      Object.entries(patch).filter(([, value]) => value !== undefined),
    );
  const mergeToolRow = (into: PresentedBlock, from: PresentedBlock) => {
    const patch = defined(from) as Partial<PresentedBlock>;
    if (from.receipt_missing) {
      // A receipt-less retained row only carries fallback values: never
      // let its name shadow the call record it joins. Its status is the
      // honest terminal marker ("complete") the retained message proves,
      // so it wins over an unfinished-looking row but never over a real
      // receipt that already landed.
      delete patch.name;
      const intoOpen = into.duration_ms == null && isRunningStatus(into.status);
      if (!intoOpen) delete patch.status;
    }
    return {
      ...into,
      ...patch,
      ...(into.duration_ms != null && patch.duration_ms == null
        ? {
            status: into.status,
            duration_ms: into.duration_ms,
            text: into.text || from.text,
          }
        : {}),
      result_loaded: true,
    };
  };
  for (const block of blocks) {
    if (
      block.kind === "assistant" &&
      !block.text &&
      !block.reasoning &&
      !block.tools?.length &&
      !block.files?.length &&
      !block.deliveries?.length &&
      !block.error &&
      !block.summary &&
      !block.truncated
    )
      continue;
    if (block.kind === "user") calls.clear();
    if (block.kind === "tool_result") {
      const index = find(block);
      if (index !== undefined) {
        rows[index] = mergeToolRow(rows[index], block);
        register(block, index);
        register(rows[index], index);
        continue;
      }
      // First sighting still registers: the matching arrival merges
      // into this row instead of rendering a second one.
      register(block, rows.length);
    }
    const tools = block.tools || [];
    if (!tools.length || block.text || block.reasoning || block.files?.length)
      rows.push({
        ...block,
        key: block.response_id || block.id,
        ...(tools.length ? { tools: [] } : {}),
      });
    for (const tool of tools) {
      const callId = tool.call_id || tool.id || "unknown";
      const occurrenceId =
        tool.occurrence_id ||
        (tool.response_id || block.response_id
          ? `${tool.response_id || block.response_id}:${callId}`
          : `t-${callId}`);
      const probe = {
        occurrence_id: occurrenceId,
        response_id: tool.response_id || block.response_id,
        call_id: callId,
        detail_id: tool.detail_id,
      };
      // The result may already be here (retained result before its call
      // on reconnect): fold the call record into it instead of pushing
      // a stuck "Running" twin. Call records only fill gaps — status,
      // text and duration belong to results.
      const at = findStrong(probe);
      if (at !== undefined) {
        const prev = rows[at];
        rows[at] = {
          ...prev,
          ...defined({
            occurrence_id: probe.occurrence_id,
            response_id: probe.response_id,
            call_id: probe.call_id,
            detail_id: probe.detail_id,
            name: tool.name,
            activity: tool.activity,
            arguments: tool.arguments,
            time: block.time,
            turn_root: block.turn_root,
            reply_to: block.reply_to,
            reply_excerpt: block.reply_excerpt,
            source: block,
          }),
        };
        register(rows[at], at);
        continue;
      }
      const row: PresentedBlock = {
        kind: "tool_result",
        id: occurrenceId,
        key: occurrenceId,
        call_id: callId,
        occurrence_id: occurrenceId,
        response_id: tool.response_id || block.response_id,
        detail_id: tool.detail_id,
        name: tool.name,
        activity: tool.activity,
        arguments: tool.arguments,
        status: tool.status,
        time: block.time,
        turn_root: block.turn_root,
        reply_to: block.reply_to,
        reply_excerpt: block.reply_excerpt,
        source: block,
        result_loaded: false,
      };
      register(row, rows.length);
      rows.push(row);
    }
  }
  return rows;
}

export type TextPart =
  { text: string } | { mention: { id: string; alt: string } };

const kMentionToken = /!\[([^\]\n]*)\]\(attachment:([A-Za-z0-9_-]+)\)/g;
const kFence = /^\s*(`{3,}|~{3,})/;

// Split @-mention tokens out of user text for inline rendering. Fenced code
// blocks are opaque: discussing the token syntax must not resolve it.
// Unresolvable ids (chip removed, or a quoted token) degrade to muted text
// in MentionFile instead of rendering as broken references.
export function splitMentionTokens(text: string): TextPart[] {
  if (text.indexOf("attachment:") < 0) return [{ text }];
  const parts: TextPart[] = [];
  const pushText = (value: string) => {
    if (!value) return;
    const last = parts.at(-1);
    if (last && "text" in last) last.text += value;
    else parts.push({ text: value });
  };
  let inFence = "";
  const lines = text.split("\n");
  for (let number = 0; number < lines.length; number++) {
    if (number > 0) pushText("\n");
    const line = lines[number];
    const fence = line.match(kFence)?.[1] ?? "";
    if (fence) {
      if (!inFence) inFence = fence[0];
      else if (fence[0] === inFence) inFence = "";
      pushText(line);
      continue;
    }
    if (inFence) {
      pushText(line);
      continue;
    }
    let index = 0;
    kMentionToken.lastIndex = 0;
    let match: RegExpExecArray | null;
    while ((match = kMentionToken.exec(line)) !== null) {
      pushText(line.slice(index, match.index));
      parts.push({ mention: { id: match[2], alt: match[1] } });
      index = match.index + match[0].length;
    }
    pushText(line.slice(index));
  }
  return parts.length ? parts : [{ text }];
}
