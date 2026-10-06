import { isFailedStatus, isRunningStatus } from "./display.ts";
import { plural } from "./quantities.ts";
import type { Block, DetailPolicy, PresentedBlock } from "./types.ts";
import { defaultDetail } from "./verbosity.ts";

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
// The verbosity level's policy then says how tool work folds.
export function presentMessages(
  blocks: Block[],
  policy: DetailPolicy = defaultDetail.policy,
): PresentedBlock[] {
  const rows: PresentedBlock[] = [];
  const responses = new Map<string, Block>();
  for (const block of blocks) {
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
      !block.error &&
      !block.summary &&
      !block.truncated
    )
      continue;
    // Routine memory outcomes show only at a level that asks for them.
    if (block.memory?.minor && !policy.minor) continue;
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
  const group = { id: `group-${key}`, key, kind: "group", label, children };
  folds.set(first, group);
  return group;
}

// The host names the calls that read as one ("Explored · 4 calls"): adjacent
// rows of one group fold under its label, and every other call keeps its row.
function foldGroups(rows: PresentedBlock[]): PresentedBlock[] {
  const folded: PresentedBlock[] = [];
  for (let begin = 0; begin < rows.length;) {
    const group = rows[begin].activity?.group;
    let end = begin + 1;
    while (group && rows[end]?.activity?.group?.id === group.id) end++;
    folded.push(
      end - begin > 1
        ? fold(rows.slice(begin, end), group!.label)
        : rows[begin],
    );
    begin = end;
  }
  return folded;
}

// What a turn's folded work did: "Worked · 14 steps · edited 2 files".
function workLabel(steps: PresentedBlock[]) {
  const calls = steps.filter((step) => step.kind !== "assistant");
  const edited = new Set(
    calls
      .filter((step) => step.activity?.category === "edit")
      .map((step) => step.view?.target || step.id),
  ).size;
  return [
    `Worked · ${plural(calls.length, "step")}`,
    edited && `edited ${plural(edited, "file")}`,
  ]
    .filter(Boolean)
    .join(" · ");
}

// A turn runs from a message of yours to its footer. Its last reply is the
// answer; the calls and interim replies before it fold into one row where
// the first of them stood. What needs you stays a row of its own: a failed
// or cancelled call, an error.
function foldTurns(rows: PresentedBlock[]): PresentedBlock[] {
  const folded: PresentedBlock[] = [];
  let turn: PresentedBlock[] = [];
  const flush = () => {
    const answer = turn.findLast((row) => row.kind === "assistant" && row.text);
    const steps: PresentedBlock[] = [];
    let at = -1;
    for (const row of turn) {
      const work =
        row !== answer &&
        !row.error &&
        !isFailedStatus(row.status) &&
        !/cancel/i.test(row.status || "") &&
        (row.kind === "tool_result" ||
          row.kind === "activity" ||
          row.kind === "assistant" ||
          (row.kind === "attachment" && row.origin === "tool"));
      if (work) {
        if (at < 0) at = folded.push(row) - 1;
        steps.push(row);
      } else folded.push(row);
    }
    if (steps.length) folded[at] = fold(steps, workLabel(steps));
    turn = [];
  };
  for (const row of rows) {
    const yours =
      row.kind === "user" ||
      (row.kind === "attachment" && row.origin !== "tool");
    if (yours || row.summary) {
      flush();
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
