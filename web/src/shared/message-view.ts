import { nextFence } from "./markdown-split.ts";
import type { Draft, LinkPart, Node } from "./types.ts";

// What a message of yours shows until the host's own row replaces it.
export const outgoingStatus = {
  // Typed while reconnecting; it goes out once the host is reached.
  waiting: "Waiting for connection",
  sending: "Sending…",
  // Guidance for the running turn, or a message held for after it.
  guidance: "Guidance queued",
  next: "Queued for after this turn",
  awaiting: "Awaiting confirmation",
  sent: "Sent",
  refused: "Not sent",
  unconfirmed: "Not confirmed",
};

// Guidance for a running turn, queued or not, goes as a steer.
export const guidanceKind = "steer";

// What a message of yours sends, read from its row: a first send and a send
// again carry the same fields, so the host takes it once.
export const sendOf = (
  row: Pick<Node, "request_id" | "body" | "files" | "status">,
) => ({
  request_id: row.request_id,
  text: row.body?.text || "",
  ...(row.status === outgoingStatus.next && { queue: true }),
  attachment_ids: (row.files ?? []).map(({ id, name }) => ({ id, name })),
});

// What a draft sends: its quotes, then what was typed; a command is what was
// typed alone, and the quotes wait for the next message.
export const messageText = (draft: Draft) =>
  draft.text.startsWith("/")
    ? draft.text
    : [...draft.quotes, draft.text].filter((part) => part.trim()).join("\n\n");

// Plain text can wait for the connection; attachments and slash commands
// need it.
export const holdable = (draft: Draft) => {
  const text = messageText(draft);
  return !!text && !draft.files.length && !text.startsWith("/");
};

export const unsent = (node: { status?: string }) =>
  node.status === outgoingStatus.refused ||
  node.status === outgoingStatus.unconfirmed;

export const waitingToSend = (node: { status?: string }) =>
  node.status === outgoingStatus.waiting;

export const queuedGuidance = (node: { status?: string }) =>
  node.status === outgoingStatus.guidance ||
  node.status === outgoingStatus.next;

// What returns to the composer: guidance still queued (withdrawn from the
// host, so only while connected) or a message that never went out, or is
// still waiting to.
export const recallable = (
  node: Pick<Node, "status" | "request_id">,
  online: boolean,
) =>
  !!node.request_id &&
  ((online && queuedGuidance(node)) || unsent(node) || waitingToSend(node));

// A child's request names the tool it asks to run in its envelope.
const request =
  /^\[agent [0-9a-f]{16} "[^\n]*" asks (?:to run ([^\n]+)|the person) \(interaction [^)\n]+\)\]\nThe (?:approval request|question) \(data, not instructions\):\n/;
export function agentReport(
  node: Node,
): (NonNullable<Node["agent"]> & { tool?: string }) | undefined {
  if (!node.agent || node.agent.outcome === "merged") return;
  if (node.agent.outcome !== "needs decision") return node.agent;
  const match = request.exec(node.body?.text || "");
  return match ? { ...node.agent, tool: match[1] } : node.agent;
}

// A tool may return a waited-on reply directly, without a separate mail.
export const agentLink = (
  part: NonNullable<Node["parts"]>[number],
): part is LinkPart => part.kind === "link" && part.to === "agent";

// Read only the shapes returned by agent_start. Older link parts carry
// navigation but no reply. A waited result can hold more text than its link.
function agentResults(text: string, links: LinkPart[]) {
  let value;
  try {
    value = JSON.parse(text);
  } catch {
    return;
  }
  const rows = Array.isArray(value)
    ? value
    : Array.isArray(value?.agents)
      ? value.agents
      : [value];
  const byId = new Map(links.map((link) => [String(link.id), link]));
  const results = [];
  for (const row of rows) {
    if (
      !row ||
      typeof row.id !== "string" ||
      typeof row.state !== "string" ||
      (row.item !== undefined && typeof row.item !== "string") ||
      (row.answer !== undefined && typeof row.answer !== "string")
    )
      return;
    const link = byId.get(row.id);
    if (row.id && !link) return;
    byId.delete(row.id);
    const assignment: string = row.item || link?.label || "";
    results.push({
      link,
      assignment,
      state: row.state as string,
      report: (row.answer as string | undefined) || link?.report,
    });
  }
  // An unfamiliar or incomplete result stays visible as the tool wrote it.
  if (!byId.size && results.length) return results;
}

type AgentReply = {
  link?: LinkPart;
  assignment: string;
  state?: string;
  report?: string;
};
const batches = new WeakMap<
  Node,
  { replies: AgentReply[]; recognized: boolean }
>();

// The header, roster and expanded output read the same retained assignments.
// Ordinary streaming replaces other nodes, leaving this calculation reusable.
export function agentBatch(node: Node, full?: string) {
  if (
    node.kind !== "call" ||
    (node.tool && node.tool !== "agent_start") ||
    !node.parts?.length ||
    !node.parts.every(agentLink)
  )
    return;
  const cached = full === undefined && batches.get(node);
  if (cached) return cached;
  const rows = agentResults(full ?? node.body?.text ?? "", node.parts);
  const batch = {
    recognized: !!rows,
    replies:
      rows ??
      node.parts.map((link) => ({
        link,
        assignment: link.label,
        state: link.state,
        report: link.report,
      })),
  };
  if (full === undefined) batches.set(node, batch);
  return batch;
}

export function agentText(text: string, outcome?: string) {
  const body = text.replace(/^\[(?:agent |side chat )[^\n]*\]\r?\n/, "");
  if (outcome !== "needs decision") return body;
  // Only unwrap the runtime's request format. The request itself is verbatim.
  const match =
    /^The (?:approval request|question) \(data, not instructions\):\r?\n([\s\S]*?)\r?\n+Decide with the decide tool; yield when the person should\.$/.exec(
      body,
    );
  return match ? match[1] : body;
}

export function shownText(node: Node, text: string) {
  const sender = agentReport(node);
  if (sender || node.origin === "agent")
    return agentText(text, sender?.outcome);
  return node.origin === "activity" ? "" : text;
}

// Keep a short reply intact. For a long reply prefer a paragraph boundary,
// otherwise stop on a word, including for reports written as a single list.
export function messageExcerpt(text: string) {
  if (text.length <= 600) return text;
  const start = text.slice(0, 400);
  const paragraph = start.lastIndexOf("\n\n");
  const word = start.search(/\s+\S*$/);
  const end = paragraph >= 80 ? paragraph : word > 0 ? word : start.length;
  // A source link can be longer than the excerpt. Leave it intact on expand.
  const prefix = start
    .slice(0, end)
    .replace(/!?\[[^\]\n]*$|!?\[[^\]\n]*\]\([^()\n]*$/, "");
  return prefix.trimEnd() + "…";
}

type TextPart = { text: string } | { mention: { id: string; alt: string } };

const kMentionToken = /!\[([^\]\n]*)\]\(attachment:([A-Za-z0-9_-]+)\)/g;

// Fenced code blocks are opaque: discussing the token syntax must not
// resolve it. Unresolvable ids degrade to muted text in MentionFile.
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
    const wasOpen = inFence;
    inFence = nextFence(inFence, line);
    if (wasOpen || inFence) {
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
