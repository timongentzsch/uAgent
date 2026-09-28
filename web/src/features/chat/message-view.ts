import { isFailedStatus, isRunningStatus } from "../../shared/display.ts";
import type { Block, PresentedBlock } from "../../shared/types.ts";

// Flat, stable, uniform rows: the host's view already holds one block per
// message and one per tool call, so a block is a row. Empty assistant
// placeholders (a response before its first delta) are dropped: the status
// line covers the live state. A tool row keeps the model call that made it.
export function presentMessages(blocks: Block[]): PresentedBlock[] {
  const rows: PresentedBlock[] = [];
  const responses = new Map<string, Block>();
  for (const block of blocks) {
    if (block.kind === "assistant" && block.response_id)
      responses.set(block.response_id, block);
    if (
      block.kind === "assistant" &&
      !block.text &&
      !block.reasoning &&
      !block.files?.length &&
      !block.deliveries?.length &&
      !block.error &&
      !block.summary &&
      !block.truncated
    )
      continue;
    // Routine memory outcomes are for the terminal's /verbose view only.
    if (block.memory?.minor) continue;
    rows.push(
      block.kind === "tool_result"
        ? {
            ...block,
            key: block.id,
            source: block.response_id
              ? responses.get(block.response_id)
              : undefined,
            result_loaded: !isRunningStatus(block.status),
          }
        : { ...block, key: block.id },
    );
  }
  return foldGroups(attachToolFiles(rows));
}

// Files a tool added to context (a browser screenshot, a read image) belong
// on the row of the call that produced them, not in a row of their own.
function attachToolFiles(rows: PresentedBlock[]): PresentedBlock[] {
  const byCall = new Map<string, number>();
  const kept: PresentedBlock[] = [];
  for (const row of rows) {
    const owner = row.source_call_ids
      ?.map((id) => byCall.get(id))
      .find((index) => index !== undefined);
    if (
      row.kind === "attachment" &&
      row.origin === "tool" &&
      owner !== undefined
    ) {
      const files = (row.files || []).filter(
        (file) => typeof file === "object",
      );
      kept[owner] = {
        ...kept[owner],
        files: [...(kept[owner].files || []), ...files],
      };
      continue;
    }
    if (row.kind === "tool_result" && row.call_id)
      byCall.set(row.call_id, kept.length);
    kept.push(row);
  }
  return kept;
}

// Intents whose consecutive calls read as one step (the native four).
const GROUPED = new Set(["explore", "research", "verify", "edit"]);

// Consecutive calls of one groupable intent fold into one row ("Explored",
// "Verified"...), as Codex and opencode do. A failure or a call with
// something to show (a file, a link) keeps its own row; one call is no group.
function foldGroups(rows: PresentedBlock[]): PresentedBlock[] {
  const folded: PresentedBlock[] = [];
  let run: PresentedBlock[] = [];
  const flush = () => {
    if (run.length > 1) {
      const key = `group-${run[0].key || run[0].id}`;
      folded.push({
        id: key,
        key,
        kind: "group",
        activity: { category: run[0].activity?.category },
        children: run,
      });
    } else folded.push(...run);
    run = [];
  };
  for (const row of rows) {
    const intent = row.activity?.category || "";
    const joins =
      row.kind === "tool_result" &&
      GROUPED.has(intent) &&
      !row.parts?.length &&
      !row.files?.length &&
      !isFailedStatus(row.status) &&
      !/cancel/i.test(row.status || "");
    if (!joins || (run.length && run[0].activity?.category !== intent)) flush();
    if (joins) run.push(row);
    else folded.push(row);
  }
  flush();
  return folded;
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
