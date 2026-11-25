import { createContext } from "preact";
import { useContext, useRef } from "preact/hooks";
import { statusOf } from "../../state/attention.ts";
import { agentReports } from "../../shared/message-view.ts";
import type { LinkPart, Node, Session } from "../../shared/types.ts";
import { Button } from "../../shared/ui.tsx";
import { agentName } from "../../shared/turn-status.tsx";
import { MessageActions } from "./message-actions.ts";

// This conversation's agents and side chats, by id.
export const AgentSessions = createContext(new Map<string, Session>());

export const sessionsById = (list: Session[]) =>
  new Map(list.map((child) => [child.id, child]));

// What the loaded nodes say each agent reported, the same object until one
// changes: the composer renders when it does, not with every streamed token.
export function useAgentReports(nodes: Node[]) {
  const held = useRef<Record<string, string>>({});
  const lines = agentReports(nodes);
  if (JSON.stringify(lines) !== JSON.stringify(held.current))
    held.current = lines;
  return held.current;
}

// What a folded delegation shows of an agent waiting on a person.
export function ReviewDecision({
  link,
  online,
}: {
  link: LinkPart;
  online: boolean;
}) {
  const child = useContext(AgentSessions).get(String(link.id));
  const { open } = useContext(MessageActions);
  if (!child || !open || statusOf(child, online) !== "waiting") return null;
  const name = agentName(child);
  return (
    <Button
      variant="quiet"
      class="review-decision small"
      aria-label={`Review decision · ${name}`}
      onClick={() => open(child.id)}
    >
      {name} · Review decision
    </Button>
  );
}
