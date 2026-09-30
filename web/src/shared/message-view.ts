import { isFailedStatus, isRunningStatus } from "./display.ts";
import type { Block, PresentedBlock } from "./types.ts";

// A block the host did not change keeps its row object, so a streamed
// frame presents only the rows it touched and the rest compare by identity.
const presented = new WeakMap<Block, PresentedBlock>();

function present(block: Block, source?: Block): PresentedBlock {
  const prior = presented.get(block);
  if (prior && prior.source === source) return prior;
  const row: PresentedBlock =
    block.kind === "tool_result"
      ? {
          ...block,
          key: block.id,
          source,
          result_loaded: !isRunningStatus(block.status),
        }
      : { ...block, key: block.id };
  presented.set(block, row);
  return row;
}

// A message of yours the host refused, or never confirmed receiving.
export const unsent = (block: Pick<Block, "status">) =>
  block.status === "Not sent" || block.status === "Not confirmed";

// Guidance for the running turn, or a message held for after it.
export const QUEUED_NEXT = "Queued for after this turn";
export const queuedGuidance = (block: Pick<Block, "status">) =>
  block.status === "Guidance queued" || block.status === QUEUED_NEXT;

// What returns to the composer: guidance still queued (withdrawn from the
// host, so only while connected) or a message that never went out.
export const recallable = (
  block: Pick<Block, "status" | "request_id">,
  online: boolean,
) => !!block.request_id && ((online && queuedGuidance(block)) || unsent(block));

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
      present(
        block,
        block.kind === "tool_result" && block.response_id
          ? responses.get(block.response_id)
          : undefined,
      ),
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

// A run of this many tool calls or more folds into one row.
const FOLD = 3;
// Each fold by its first row: a run whose rows are unchanged keeps its
// group, so as rows stream only the last run is refolded (and re-rendered).
const folds = new WeakMap<PresentedBlock, PresentedBlock>();

// Consecutive tool calls fold into one row ("Ran 4 commands · edited 2
// files"), as Codex and opencode do. A failure or a call with something to
// show (a file, a link) keeps its own row and ends the run. The group takes
// its first row's key, so the transcript keeps its place when a run folds.
function foldGroups(rows: PresentedBlock[]): PresentedBlock[] {
  const folded: PresentedBlock[] = [];
  let run: PresentedBlock[] = [];
  const flush = () => {
    const first = run[0];
    if (run.length < FOLD) folded.push(...run);
    else {
      const prior = folds.get(first);
      const same =
        prior?.children?.length === run.length &&
        prior.children.every((row, index) => row === run[index]);
      const key = first.key || first.id;
      const group = same
        ? prior
        : { id: `group-${key}`, key, kind: "group", children: run };
      folds.set(first, group);
      folded.push(group);
    }
    run = [];
  };
  for (const row of rows) {
    const joins =
      row.kind === "tool_result" &&
      !row.parts?.length &&
      !row.files?.length &&
      !isFailedStatus(row.status) &&
      !/cancel/i.test(row.status || "");
    if (joins) run.push(row);
    else {
      flush();
      folded.push(row);
    }
  }
  flush();
  return folded;
}

type TextPart = { text: string } | { mention: { id: string; alt: string } };

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
