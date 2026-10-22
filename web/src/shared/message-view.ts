import { assetsOf, isFailedStatus, isRunningStatus } from "./display.ts";
import { duration } from "./duration.ts";
import { plural } from "./quantities.ts";
import type {
  AssistantBlock,
  Block,
  DetailPolicy,
  PresentedBlock,
  UserBlock,
} from "./types.ts";
import { defaultDetail } from "./verbosity.ts";

// A block the host did not change keeps its row object, so a streamed
// frame presents only the rows it touched and the rest compare by identity.
const presented = new WeakMap<Block, PresentedBlock>();

function present(block: Block, source?: AssistantBlock): PresentedBlock {
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

// What a message of yours shows until the host's own row replaces it.
export const outgoingStatus = {
  sending: "Sending…",
  // Guidance for the running turn, or a message held for after it.
  guidance: "Guidance queued",
  next: "Queued for after this turn",
  awaiting: "Awaiting confirmation",
  sent: "Sent",
  refused: "Not sent",
  unconfirmed: "Not confirmed",
};

// A message of yours the host refused, or never confirmed receiving.
export const unsent = (block: Pick<Block, "status">) =>
  block.status === outgoingStatus.refused ||
  block.status === outgoingStatus.unconfirmed;

export const queuedGuidance = (block: Pick<Block, "status">) =>
  block.status === outgoingStatus.guidance ||
  block.status === outgoingStatus.next;

// What returns to the composer: guidance still queued (withdrawn from the
// host, so only while connected) or a message that never went out.
export const recallable = (
  block: Pick<UserBlock, "status" | "request_id">,
  online: boolean,
) => !!block.request_id && ((online && queuedGuidance(block)) || unsent(block));

// Flat, stable, uniform rows: the host's view already holds one block per
// message and one per tool call, so a block is a row. Empty assistant
// placeholders (a response before its first delta) are dropped: the status
// line covers the live state. A tool row keeps the model call that made it.
// The verbosity level's policy then says how tool work folds.
// A chat participant answers "PASS" or "WAIT" when it has nothing to say
// now. Such an answer is nobody's to read, and neither is one that may still
// become it: while a reply streams and could yet be one of the two words,
// it has no row, so nothing appears only to be taken away.
const UNSAID = ["PASS", "PASS.", "WAIT", "WAIT."];
function unsaid(block: Block, chat: boolean) {
  if (block.silent) return true;
  if (!chat || block.kind !== "assistant") return false;
  const text = (block.text || "").trim();
  return block.streaming
    ? UNSAID.some((word) => word.startsWith(text))
    : UNSAID.includes(text);
}

// With `chat`, the conversation is a panel's chat with members.
export function presentMessages(
  blocks: Block[],
  policy: DetailPolicy = defaultDetail.policy,
  chat = false,
): PresentedBlock[] {
  const rows: PresentedBlock[] = [];
  const responses = new Map<string, AssistantBlock>();
  for (const block of blocks) {
    if (unsaid(block, chat)) continue;
    if (block.kind === "assistant" && block.response_id)
      responses.set(block.response_id, block);
    // Thinking is content only at a level that shows it: a reply holding
    // nothing else would otherwise be an empty row with its margins.
    if (
      block.kind === "assistant" &&
      !block.text &&
      !(block.reasoning && policy.reasoning !== "hidden") &&
      !block.files?.length &&
      !block.deliveries?.length &&
      !block.truncated
    )
      continue;
    // Routine memory outcomes show only at a level that asks for them.
    if (block.kind === "activity" && block.memory?.minor && !policy.minor)
      continue;
    rows.push(
      present(
        block,
        block.kind === "tool_result" && block.response_id
          ? responses.get(block.response_id)
          : undefined,
      ),
    );
  }
  const kept = attachToolFiles(rows);
  return policy.work === "turn"
    ? foldTurns(kept)
    : policy.work === "groups"
      ? foldGroups(kept)
      : kept;
}

// Files a tool added to context (a browser screenshot, a read image) belong
// on the row of the call that produced them, not in a row of their own.
function attachToolFiles(rows: PresentedBlock[]): PresentedBlock[] {
  const byCall = new Map<string, number>();
  const kept: PresentedBlock[] = [];
  for (const row of rows) {
    const owner =
      row.kind === "attachment"
        ? row.source_call_ids
            ?.map((id) => byCall.get(id))
            .find((index) => index !== undefined)
        : undefined;
    if (
      row.kind === "attachment" &&
      row.origin === "tool" &&
      owner !== undefined
    ) {
      const files = assetsOf(row.files);
      const call = kept[owner];
      if (call.kind === "tool_result")
        kept[owner] = { ...call, files: [...(call.files || []), ...files] };
      continue;
    }
    if (row.kind === "tool_result" && row.call_id)
      byCall.set(row.call_id, kept.length);
    kept.push(row);
  }
  return kept;
}

// Each fold by its first row: a fold whose rows are unchanged keeps its
// group, so as rows stream only the last one is refolded (and re-rendered).
const folds = new WeakMap<PresentedBlock, PresentedBlock>();

// Rows as one row that opens to them. It takes its first row's key, so the
// transcript keeps its place when rows fold.
function fold(children: PresentedBlock[], label: string): PresentedBlock {
  const first = children[0];
  const prior = folds.get(first);
  if (
    prior?.label === label &&
    prior.children?.length === children.length &&
    prior.children.every((row, index) => row === children[index])
  )
    return prior;
  const key = first.key || first.id;
  const group: PresentedBlock = {
    id: `group-${key}`,
    key,
    kind: "group",
    label,
    children,
  };
  folds.set(first, group);
  return group;
}

// The host names the calls that read as one ("Explored · 4 calls"): adjacent
// rows of one group fold under its label, and every other call keeps its row.
const groupOf = (row: PresentedBlock) =>
  "activity" in row ? row.activity?.group : undefined;
function foldGroups(rows: PresentedBlock[]): PresentedBlock[] {
  const folded: PresentedBlock[] = [];
  for (let begin = 0; begin < rows.length;) {
    const group = groupOf(rows[begin]);
    let end = begin + 1;
    while (group && rows[end] && groupOf(rows[end])?.id === group.id) end++;
    folded.push(
      end - begin > 1
        ? fold(rows.slice(begin, end), group!.label)
        : rows[begin],
    );
    begin = end;
  }
  return folded;
}

// What a turn's folded work did: "Worked · 14 steps · edited 2 files · 1m 12s".
// How long the turn took is known once it has ended.
function workLabel(steps: PresentedBlock[], took?: number) {
  const calls = steps.filter((step) => step.kind !== "assistant");
  const edited = new Set(
    calls
      .filter(
        (step) => "activity" in step && step.activity?.category === "edit",
      )
      .map((step) => ("view" in step && step.view?.target) || step.id),
  ).size;
  return [
    `Worked · ${plural(calls.length, "step")}`,
    edited && `edited ${plural(edited, "file")}`,
    took !== undefined && duration(took),
  ]
    .filter(Boolean)
    .join(" · ");
}

// A turn runs from a message of yours to its footer. Its last reply after
// its last call is the answer; the calls and interim replies before it fold
// into one row where the first of them stood. What is for you stays a row
// of its own: a failed or cancelled call, an error, a file the agent handed
// you.
function foldTurns(rows: PresentedBlock[]): PresentedBlock[] {
  const folded: PresentedBlock[] = [];
  let turn: PresentedBlock[] = [];
  const flush = (took?: number) => {
    const lastCall = turn.findLastIndex((row) => row.kind === "tool_result");
    const answer = turn.findLast(
      (row, index) => index > lastCall && row.kind === "assistant" && row.text,
    );
    const steps: PresentedBlock[] = [];
    let at = -1;
    for (const row of turn) {
      const work =
        row !== answer &&
        !isFailedStatus(row.status) &&
        !/cancel/i.test(row.status || "") &&
        !("parts" in row && row.parts?.some((part) => part.kind === "file")) &&
        (row.kind === "tool_result" ||
          row.kind === "activity" ||
          row.kind === "assistant" ||
          (row.kind === "attachment" && row.origin === "tool"));
      if (work) {
        if (at < 0) at = folded.push(row) - 1;
        steps.push(row);
      } else folded.push(row);
    }
    if (steps.length) folded[at] = fold(steps, workLabel(steps, took));
    turn = [];
  };
  for (const row of rows) {
    const yours =
      row.kind === "user" ||
      (row.kind === "attachment" && row.origin !== "tool");
    const summary = row.kind === "turn_summary" ? row.summary : undefined;
    if (yours || summary) {
      flush(summary?.duration_ms);
      folded.push(row);
    } else turn.push(row);
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
