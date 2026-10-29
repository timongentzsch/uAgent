import type { Node } from "./types.ts";

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
export const unsent = (node: { status?: string }) =>
  node.status === outgoingStatus.refused ||
  node.status === outgoingStatus.unconfirmed;

export const queuedGuidance = (node: { status?: string }) =>
  node.status === outgoingStatus.guidance ||
  node.status === outgoingStatus.next;

// What returns to the composer: guidance still queued (withdrawn from the
// host, so only while connected) or a message that never went out.
export const recallable = (
  node: Pick<Node, "status" | "request_id">,
  online: boolean,
) => !!node.request_id && ((online && queuedGuidance(node)) || unsent(node));

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
